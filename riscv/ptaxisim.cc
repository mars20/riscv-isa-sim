#include "ptaxisim.h"

#include "mmu.h"
#include "disasm.h"
#include "decode.h"
#include <vector>

// From https://stackoverflow.com/questions/3219393/stdlib-and-colored-output-in-c
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define OPCODE_LOAD      (0b0000011)
#define OPCODE_LOADFP    (0b0000111)
#define OPCODE_MISCMEM   (0b0001111)
#define OPCODE_OPIMM     (0b0010011)
#define OPCODE_AUIPC     (0b0010111)
#define OPCODE_OPIMM32   (0b0011011)
#define OPCODE_STORE     (0b0100011)
#define OPCODE_STOREFP   (0b0100111)
#define OPCODE_AMO       (0b0101111)
#define OPCODE_OP        (0b0110011)
#define OPCODE_LUI       (0b0110111)
#define OPCODE_OP32      (0b0111011)
#define OPCODE_MADD      (0b1000011)
#define OPCODE_MSUB      (0b1000111)
#define OPCODE_NMSUB     (0b1001011)
#define OPCODE_NMADD     (0b1001111)
#define OPCODE_OPFP      (0b1010011)
#define OPCODE_BRANCH    (0b1100011)
#define OPCODE_JALR      (0b1100111)
#define OPCODE_JAL       (0b1101111)
#define OPCODE_SYSTEM    (0b1110011)

#define OPCODE_TAGCMD    (0b0001011)
#define OPCODE_TAGPOLICY (0b0101011)

#define REG_SP 2

#define TAG_RET_FROM_JAL 1
#define TAG_RET_FROM_MEM 2
#define SR_TAG_SHIFT 9
#define PTAXI_DEBUG_MODE_CONTEXT_ID 42 // Any number > 0 is fine here, just for debugging purpose.

// Adapted from https://stackoverflow.com/questions/1941307/c-debug-print-macros
#define PTAXI_VERBOSE
//#define PTAXI_DEBUG
#ifdef PTAXI_VERBOSE
#define DPRINTF(fmt, args...) printf(fmt, ## args)
#else
#define DPRINTF(fmt, args...)
#endif

#ifdef PTAXI_DEBUG
#define DDPRINTF(fmt, args...) printf(fmt, ## args)
#else
#define DDPRINTF(fmt, args...)
#endif

ptaxi_sim_t::ptaxi_sim_t() {
  struct ptaxi_context_state_t default_state;
  default_state.is_enable = false;
  default_state.priv_bits = 0;
  default_state.lowest_sp_addr = 0;
  states.push_back(default_state);
}

ptaxi_insn_type_t ptaxi_sim_t::get_insn_type(insn_t insn) {
  switch (insn.opcode()) {
  case OPCODE_LOAD:
    if (insn.rm() == 3) {
      return PTAXI_INSN_TYPE_LOAD64;
    } else {
      return PTAXI_INSN_TYPE_LOAD;
    }
  case OPCODE_STORE:
    if (insn.rm() == 3) {
      return PTAXI_INSN_TYPE_STORE64;
    } else {
      return PTAXI_INSN_TYPE_STORE;
    }
  case OPCODE_OP:
    return PTAXI_INSN_TYPE_OP;
  case OPCODE_OPIMM:
    if (insn.rm() == 0 && insn.i_imm() == 0) {
      return PTAXI_INSN_TYPE_COPY;
    }
    return PTAXI_INSN_TYPE_OPIMM;
  case OPCODE_JAL:
    return PTAXI_INSN_TYPE_JAL;
  case OPCODE_JALR:
    // rs1 == X_RA (X_RA = 1)
    if (insn.i_imm() == 0 && insn.rs1() == 1 && insn.rm() == 0 && insn.rd() == 0) {
      return PTAXI_INSN_TYPE_RETURN;
    }
    return PTAXI_INSN_TYPE_JALR;
  case OPCODE_TAGCMD:
    return PTAXI_INSN_TYPE_TAGCMD;
  case OPCODE_TAGPOLICY:
    return PTAXI_INSN_TYPE_TAGPOLICY;

  default:
    return PTAXI_INSN_TYPE_UNKNOWN;
  }
}

size_t ptaxi_sim_t::get_ptaxi_context_id(processor_t *p, bool add_if_needed) {
  size_t context_id;
  if (benchmark_mode) {
    context_id = PTAXI_DEBUG_MODE_CONTEXT_ID;
  } else {
    context_id = (p->get_pcr(CSR_STATUS) & SR_TAG) >> SR_TAG_SHIFT;
  }
  if (add_if_needed && context_id == 0) {
    reg_t old = p->get_pcr(CSR_STATUS);
    context_id = states.size();
    if (context_id >= (1 << 7)) {
      DPRINTF("Context ID Full...\n");
      return 0;
    }
    p->set_pcr(CSR_STATUS, old | (context_id << SR_TAG_SHIFT));
  }
  while (context_id >= states.size()) {
    states.push_back(states[0]);
  }
  return context_id;
}

void print_insn(processor_t *p, const char *str, insn_t insn) {
  disassembler_t* disas = p->get_disassembler();
  printf("\x1b[32m%s: %-25s", str, disas->disassemble(insn).c_str());
  printf("RS1: %2lu, RS2: %2lu, IMM: %8ld, RS1VAL: %8lu (0x%8lx), RS2VAL: %8lu (0x%8lx)\x1b[0m\n",
      insn.rs1(), insn.rs2(), insn.i_imm(), RS1, RS1, RS2, RS2);
}

std::pair<ptaxi_action_t, int> ptaxi_sim_t::determine_ptaxi_action(processor_t *p, insn_t insn,
    reg_t pc) {
  size_t context_id = get_ptaxi_context_id(p, false);
  if (context_id == 0 || !states[context_id].is_enable || IS_SUPERVISOR) {
    return std::make_pair(0, -2);
  }

  ptaxi_insn_type_t insn_type = get_insn_type(insn);
  ptaxi_action_t action = 0;
  uint8_t tag_arg1 = 0, tag_arg2 = 0, tag_out = 0, tag_out_updated = 0;
  bool is_load_tag_arg1 = false, is_load_tag_arg2 = false, is_load_tag_out = false;
  bool has_match = false;
  size_t i;

  for (i = 0; i < states[context_id].policy_contexts.size(); i++) {
    struct ptaxi_policy_t policy = states[context_id].policy_contexts[i].policy;
    bool match = (insn_type == policy.insn_type);
    if (match && policy.rs1_mask) {
      match = match && ((insn.rs1() & policy.rs1_mask) == policy.rs1_match);
    }
    if (match && policy.rs2_mask) {
      match = match && ((insn.rs2() & policy.rs2_mask) == policy.rs2_match);
    }

    if (match && policy.priv_mask) {
      match = match && ((states[context_id].priv_bits & policy.priv_mask) == policy.priv_match);
    }

    if (match && policy.rs1val_mask) {
      match = match && ((RS1 & policy.rs1val_mask) == policy.rs1val_match);
    }
    if (match && policy.rs2val_mask) {
      match = match && ((RS2 & policy.rs2val_mask) == policy.rs2val_match);
    }

    if (match && policy.tag_arg1_mask) {
      if (!is_load_tag_arg1) {
        is_load_tag_arg1 = true;
        tag_arg1 = get_or_set_tag(p, insn, pc, insn_type, INSN_ARG1, false, 0);
        if (benchmark_mode) {
          counters.tag_read++;
        }
      }
      match = match && ((tag_arg1 & policy.tag_arg1_mask) == policy.tag_arg1_match);
    }
    if (match && policy.tag_arg2_mask) {
      if (!is_load_tag_arg2) {
        is_load_tag_arg2 = true;
        tag_arg2 = get_or_set_tag(p, insn, pc, insn_type, INSN_ARG2, false, 0);
        if (benchmark_mode) {
          counters.tag_read++;
        }
      }
      match = match && ((tag_arg2 & policy.tag_arg2_mask) == policy.tag_arg2_match);
    }

    if (match && (policy.tag_out_mask || policy.tag_out_tomodify)) {
      if (!is_load_tag_out) {
        is_load_tag_out = true;
        tag_out = get_or_set_tag(p, insn, pc, insn_type, INSN_OUT, false, 0);
        tag_out_updated = tag_out;
        if (benchmark_mode) {
          counters.tag_read++;
        }
      }
      match = match && ((tag_out & policy.tag_out_mask) == policy.tag_out_match);
    }

    if (match) {
      has_match = true;
      struct ptaxi_policy_context_t &policy_context = states[context_id].policy_contexts[i];
      policy_context.match_count++;
      if (policy_context.match_count <= policy_context.policy.ignore_count) {
        continue;
      }
      tag_out_updated = ((tag_out_updated & (~policy.tag_out_tomodify)) | policy.tag_out_set);
      if (policy.priv_tomodify) {
        states[context_id].priv_bits = ((states[context_id].priv_bits & (~policy.priv_tomodify))
            | policy.priv_set);
        DPRINTF("Priv Bits set to %d\n", (int) states[context_id].priv_bits);
      }


      action |= policy.action;
      if (policy.action == PTAXI_ACTION_BLOCK || policy.action == PTAXI_ACTION_ALLOW) {
        break;
      }
    }
  }

  bool real_tag_update = false;
  if (is_load_tag_out && (tag_out != tag_out_updated)) {
    real_tag_update = true;
    get_or_set_tag(p, insn, pc, insn_type, INSN_OUT, true, tag_out_updated);
  }

  if (benchmark_mode) {
    uint8_t bits = (((uint8_t) is_load_tag_arg1) << 3) + (((uint8_t) is_load_tag_arg2) << 2) +
        (((uint8_t) is_load_tag_out) << 1) + (uint8_t) real_tag_update;
    if (real_tag_update) {
      counters.tag_write++;
    }
    counters.insns++;
    counters.needs[bits]++;
    if (has_match) {
      counters.match_insns++;
    }
  }

  return std::make_pair(action, i);
}

reg_t ptaxi_sim_t::execute_insn(processor_t *p, reg_t pc, insn_fetch_t fetch) {
  insn_t insn = fetch.insn;
  uint64_t before_tag_val = 0;
  ptaxi_insn_type_t insn_type = get_insn_type(insn);
  if (insn_type == PTAXI_INSN_TYPE_TAGCMD && insn.rd() != 0) {
    before_tag_val = TAG_S2;
  }

  std::pair<ptaxi_action_t, int> paction = determine_ptaxi_action(p, insn, pc);
  ptaxi_action_t action = paction.first;
  disassembler_t* disas = p->get_disassembler();

  if (!benchmark_mode) {
    if (action & PTAXI_ACTION_DEBUG_LINE) {
      printf(ANSI_COLOR_CYAN "%p: %-25s DEBUG\n" ANSI_COLOR_RESET, (void *) pc,
          disas->disassemble(insn).c_str());
    }

    if (action & PTAXI_ACTION_DEBUG_DETAIL) {
      size_t context_id = get_ptaxi_context_id(p, true);
      printf(ANSI_COLOR_MAGENTA "PTAXI_ACTION_DEBUG_DETAIL: %s\n",
          disas->disassemble(insn).c_str());
      printf("PC: %lx, Exit Rule: %d, Context ID: %lu\n", pc, paction.second, context_id);
      print_insn(p, "INSN", fetch.insn);
      print_policies(context_id);
      printf(ANSI_COLOR_RESET);
    }

    if (action & PTAXI_ACTION_BLOCK) {
      size_t context_id = get_ptaxi_context_id(p, true);

      printf(ANSI_COLOR_MAGENTA "PTAXI_ACTION_BLOCK: %s\n" ANSI_COLOR_RESET,
          disas->disassemble(insn).c_str());
      print_policies(context_id);
      throw trap_tag_violation();
      return pc;
    }
  }

  if (action & PTAXI_ACTION_GC) {
    size_t context_id = get_ptaxi_context_id(p, false);
    uint64_t cur_sp = STATE.XPR[REG_SP];
    uint64_t lowest = states[context_id].lowest_sp_addr;
    DDPRINTF(ANSI_COLOR_MAGENTA "%10p: %-25s GCSTAR (--) = %p %p\n" ANSI_COLOR_RESET, (void *) pc,
      disas->disassemble(insn).c_str(), (void *) cur_sp, (void *) lowest);
    uint64_t clean_from = lowest - 8;
    uint64_t clean_to = cur_sp - 8;
    uint64_t clean_at;
    for(clean_at = clean_from; clean_at < clean_to; clean_at += 8) {
      MMU.store_tagged_uint64(clean_at, 0, 0);
    }

    DDPRINTF(ANSI_COLOR_GREEN "CLEAN FROM %p to %p\n" ANSI_COLOR_RESET, (void *) clean_from,
        (void *) clean_to);

    states[context_id].lowest_sp_addr = cur_sp;
  }


  if (insn_type == PTAXI_INSN_TYPE_TAGCMD && insn.rd() != 0) {
    if (action & PTAXI_ACTION_GETTAG) {
      DDPRINTF(ANSI_COLOR_CYAN "%10p: %-25s GETTAG (%2d) = %d\n" ANSI_COLOR_RESET, (void *) pc,
          disas->disassemble(insn).c_str(), (int) insn.rs2(), (int) before_tag_val);
      WRITE_RD(before_tag_val);
    } else {
      WRITE_RD(RS2);
    }
  }

  reg_t res = fetch.func(p, insn, pc);

  if(insn.rd() == REG_SP && !IS_SUPERVISOR) {
    size_t context_id = get_ptaxi_context_id(p, false);
    if (context_id != 0) {
      uint64_t cur_sp = STATE.XPR[REG_SP];
      DDPRINTF(ANSI_COLOR_CYAN "%10p: %-25s MODISP (--) = %p\n" ANSI_COLOR_RESET, (void *) pc,
          disas->disassemble(insn).c_str(), (void *) cur_sp);
      if (cur_sp < states[context_id].lowest_sp_addr || states[context_id].lowest_sp_addr == 0) {
        states[context_id].lowest_sp_addr = cur_sp;
        DDPRINTF(ANSI_COLOR_BLUE "%10p: %-25s LOWEST (--) = %p\n" ANSI_COLOR_RESET, (void *) pc,
                  disas->disassemble(insn).c_str(), (void *) cur_sp);
      }
    }
  }

  return res;
}

void ptaxi_sim_t::print_policies(size_t context_id) {
  printf("Policy Count: %lu\n------\n", states[context_id].policy_contexts.size());
  for (size_t i = 0; i < states[context_id].policy_contexts.size(); i++) {
    struct ptaxi_policy_context_t policy_context = states[context_id].policy_contexts[i];
    printf("%3lu |%5d |%3d%3d |%3lu\n", i, (int) policy_context.policy.insn_type,
        (int) policy_context.policy.rs1val_match, (int) policy_context.policy.action,
        policy_context.match_count);
  }
  printf("------\n");
}

void ptaxi_sim_t::add_policy(processor_t *p, uint64_t a, uint64_t b, uint64_t c) {
  size_t context_id = get_ptaxi_context_id(p, true);
  union ptaxi_policy_serialized ps;
  ps.regs.a = a;
  ps.regs.b = b;
  ps.regs.c = c;

  struct ptaxi_policy_context_t policy_context;
  policy_context.policy = ps.policy;
  policy_context.match_count = 0;
  states[context_id].policy_contexts.push_back(policy_context);
}

void ptaxi_sim_t::run_tag_command(processor_t *p, uint64_t cmd) {
  size_t context_id = get_ptaxi_context_id(p, true);
  if (cmd == 0) {
    DPRINTF(ANSI_COLOR_CYAN "Enforcing.. Context Id = %d\n" ANSI_COLOR_RESET, (int) context_id);
    states[context_id].is_enable = true;
  } else {
    DPRINTF(ANSI_COLOR_YELLOW "TAG COMMAND %lu\n" ANSI_COLOR_RESET, cmd);
  }
  #ifdef PTAXI_VERBOSE
    print_policies(context_id);
  #endif
}

uint8_t ptaxi_sim_t::get_or_set_tag(processor_t *p, insn_t insn, reg_t pc,
    ptaxi_insn_type_t insn_type, insn_var_type_t var_type, bool set_tag, uint8_t tag_val) {
  bool is_invalid = false, is_mem = false;
  uint8_t reg;
  uint64_t addr;

  switch (insn_type) {
  case PTAXI_INSN_TYPE_LOAD64: // arg1 = MEM, arg2 = N/A, out = REG
  case PTAXI_INSN_TYPE_LOAD:
    if (var_type == INSN_ARG1) {
      is_mem = true;
      addr = RS1+ insn.i_imm();
    } else if (var_type == INSN_ARG2) {
      is_invalid = true;
    } else {
      reg = insn.rd();
    }
    break;
    case PTAXI_INSN_TYPE_STORE64: // arg1 = REG, arg2 = N/A, out = MEM
    case PTAXI_INSN_TYPE_STORE:
    if (var_type == INSN_ARG1) {
      reg = insn.rs2();
    } else if (var_type == INSN_ARG2) {
      is_invalid = true;
    } else {
      is_mem = true;
      addr = RS1 + insn.s_imm();
    }
    break;
    case PTAXI_INSN_TYPE_TAGCMD:
    case PTAXI_INSN_TYPE_OP: // arg1 = REG1, arg2 = REG2, out = REGOUT
    if (var_type == INSN_ARG1) {
      reg = insn.rs1();
    } else if (var_type == INSN_ARG2) {
      reg = insn.rs2();
    } else {
      reg = insn.rd();
    }
    break;
    case PTAXI_INSN_TYPE_OPIMM: // arg1 = REG1, arg2 = N/A, out = REGOUT
    case PTAXI_INSN_TYPE_COPY:
    if (var_type == INSN_ARG1) {
      reg = insn.rs1();
    } else if (var_type == INSN_ARG2) {
      is_invalid = true;
    } else {
      reg = insn.rd();
    }
    break;
    case PTAXI_INSN_TYPE_JAL: // arg1 = TARGET, arg2 = n/a, arg3 = REGOUT
    if (var_type == INSN_ARG1) {
      /*is_mem = true;
       addr = pc + insn.uj_imm();*/
      is_invalid = true;
    } else if (var_type == INSN_ARG2) {
      is_invalid = true;
    } else {
      reg = insn.rd();
    }
    break;
    case PTAXI_INSN_TYPE_JALR: // arg1 = REG1, arg2 = TARGET, arg3 = REGOUT
    case PTAXI_INSN_TYPE_RETURN:
    if (var_type == INSN_ARG1) {
      reg = insn.rs1();
    } else if (var_type == INSN_ARG2) {
      is_mem = true;
      addr = (RS1 + insn.i_imm()) & ~reg_t(1);
    } else {
      reg = insn.rd();
    }
    break;
    default:
    is_invalid = true;
    break;
  }

  if (is_invalid) {
    DPRINTF("get_or_set_tag: ISINVALID TRAP!\n");
    DPRINTF("GET OR SET TAG %lx %d %d %d\n", insn.bits(), (int) insn_type, (int) var_type,
        (int) set_tag);
    throw trap_tag_violation();
    return 0;
  }
  disassembler_t* disas = p->get_disassembler();

  if (is_mem) {
    if (set_tag) {
      store_tag_to_mem(p, addr, insn.rm(), tag_val);
      DDPRINTF(ANSI_COLOR_CYAN "%10p: %-25s SETMEM (%p) = %d\n" ANSI_COLOR_RESET, (void *) pc,
          disas->disassemble(insn).c_str(), (void *) addr, (int) tag_val);

    } else {
      uint8_t tag_val_from_mem = load_tag_from_mem(p, addr, insn.rm());
      DDPRINTF(ANSI_COLOR_CYAN "%10p: %-25s LOADTG (%p) = %d\n" ANSI_COLOR_RESET, (void *) pc,
          disas->disassemble(insn).c_str(), addr, (int) tag_val_from_mem);
      return tag_val_from_mem;
    }
  } else {
    if (reg == 0) {
      return 0;
    }
    if (set_tag) {
      DDPRINTF(ANSI_COLOR_CYAN "%10p: %-25s SETREG (%2d) = %d\n" ANSI_COLOR_RESET, (void *) pc,
          disas->disassemble(insn).c_str(), (int) reg, (int) tag_val);
      STATE.XPR.write_tag(reg, tag_val);
    } else {
      return STATE.XPR.read_tag(reg);
    }
  }
  return 0;
}

uint8_t ptaxi_sim_t::load_tag_from_mem(processor_t *p, uint64_t addr, uint8_t rm) {
  switch (rm) {
  case 0: // LB
    return MMU.load_tag_only_int8(addr);
  case 1: // LH
    return MMU.load_tag_only_int16(addr);
  case 2: // LW
    return MMU.load_tag_only_int32(addr);
  case 3: // LD
    return MMU.load_tag_only_int64(addr);
  case 4: // LBU
    return MMU.load_tag_only_uint8(addr);
  case 5: // LHU
    return MMU.load_tag_only_uint16(addr);
  case 6: // LWU
    return MMU.load_tag_only_uint32(addr);
  default:
    DPRINTF("get_or_set_tag: ISINVALID TRAP2!");
    throw trap_tag_violation();
    return 0;
  }
}

void ptaxi_sim_t::store_tag_to_mem(processor_t *p, uint64_t addr, uint8_t rm, uint64_t val) {
  switch (rm) {
  case 0: // SB
    MMU.store_tag_only_uint8(addr, val);
    break;
  case 1: // SH
    MMU.store_tag_only_uint16(addr, val);
    break;
  case 2: // SW
    MMU.store_tag_only_uint32(addr, val);
    break;
  case 3: // SD
    MMU.store_tag_only_uint64(addr, val);
    break;
  default:
    DPRINTF("get_or_set_tag: ISINVALID TRAP3!");
    throw trap_tag_violation();
    break;
  }
}

void ptaxi_sim_t::start_benchmark(processor_t *p) {
  if (benchmark_mode) {
    return;
  }
  DPRINTF(ANSI_COLOR_GREEN "Start Benchmark..\n" ANSI_COLOR_RESET);
  memset(&counters, 0, sizeof(counters));
  benchmark_mode = true;
}

void ptaxi_sim_t::stop_benchmark(processor_t *p) {
  if (!benchmark_mode) {
    return;
  }
  DPRINTF(ANSI_COLOR_GREEN "Stop Benchmark..\n" ANSI_COLOR_RESET);
  print_policies(PTAXI_DEBUG_MODE_CONTEXT_ID);
  printf("RESULT,%lu,%lu,%lu,%lu", counters.insns, counters.match_insns, counters.tag_read, counters.tag_write);
  for(int i = 0; i < 16; i++) {
    printf(",%lu", counters.needs[i]);
  }
  printf("\n");
  benchmark_mode = false;

}
