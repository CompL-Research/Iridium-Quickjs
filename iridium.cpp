#include "quickjs-libc.h"
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
extern "C" {
#include "./cutils.h"
#include "./quickjs-opcode.h"
#include "./quickjs_expose.h"
#include "iridium.h"
}

#include "json.hpp"
#include <fstream>
#include <iostream>

#include <assert.h>
#include <chrono>
#include <ctype.h>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define LINKING_DUMP_FUNCTION

using json = nlohmann::json;

enum IridiumDataType { NUMBER, STRING, BOOLEAN, NULLPTR };

struct IridiumFlag {
  std::string name;
  enum IridiumDataType datatype; // BOOLEAN, NUMBER, STRING, NULLPTR

  // Storing values directly (no unions for std::string)
  bool boolean_val;
  double number_val;
  std::string string_val;
};

struct IridiumSEXP {
  std::string tag;
  int numArgs;
  IridiumSEXP **args;
  int numFlags;
  IridiumFlag **flags;
};

class ScopedTimer {
public:
  ScopedTimer(const std::string &name)
      : name_(name), start_(std::chrono::high_resolution_clock::now()) {}

  ~ScopedTimer() {
    auto end = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start_);
    std::cout << name_ << " took " << duration.count() << " ms\n";
  }

private:
  std::string name_;
  std::chrono::high_resolution_clock::time_point start_;
};

using namespace std;

#define check_dump_flag(ctx, flag)                                             \
  ((JS_GetDumpFlags(ctx->rt) & (flag + 0)) == (flag + 0))

#define OP_DEFINE_METHOD_METHOD 0
#define OP_DEFINE_METHOD_GETTER 1
#define OP_DEFINE_METHOD_SETTER 2
#define OP_DEFINE_METHOD_ENUMERABLE 4

#define JS_STACK_SIZE_MAX 65534

typedef enum OPCodeFormat {
#define FMT(f) OP_FMT_##f,
#define DEF(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef DEF
#undef FMT
} OPCodeFormat;

typedef enum {
  /* XXX: add more variable kinds here instead of using bit fields */
  JS_VAR_NORMAL,
  JS_VAR_FUNCTION_DECL,     /* lexical var with function declaration */
  JS_VAR_NEW_FUNCTION_DECL, /* lexical var with async/generator
                               function declaration */
  JS_VAR_CATCH,
  JS_VAR_FUNCTION_NAME, /* function expression name */
  JS_VAR_PRIVATE_FIELD,
  JS_VAR_PRIVATE_METHOD,
  JS_VAR_PRIVATE_GETTER,
  JS_VAR_PRIVATE_SETTER,        /* must come after JS_VAR_PRIVATE_GETTER */
  JS_VAR_PRIVATE_GETTER_SETTER, /* must come after JS_VAR_PRIVATE_SETTER */
} JSVarKindEnum;

typedef enum OPCodeEnum {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) OP_##id,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
  OP_COUNT, /* excluding temporary opcodes */
  /* temporary opcodes : overlap with the short opcodes */
  OP_TEMP_START = OP_nop + 1,
  OP___dummy = OP_TEMP_START - 1,
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f)
#define def(id, size, n_pop, n_push, f) OP_##id,
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
  OP_TEMP_END,
} OPCodeEnum;

#define ENABLE_DUMPS

typedef struct JSOpCode {
#ifdef ENABLE_DUMPS // JS_DUMP_BYTECODE_*
  const char *name;
#endif
  uint8_t size; /* in bytes */
  /* the opcodes remove n_pop items from the top of the stack, then
     pushes n_push items */
  uint8_t n_pop;
  uint8_t n_push;
  uint8_t fmt;
} JSOpCode;

static const JSOpCode opcode_info[OP_COUNT + (OP_TEMP_END - OP_TEMP_START)] = {
#define FMT(f)
#ifdef ENABLE_DUMPS // JS_DUMP_BYTECODE_*
#define DEF(id, size, n_pop, n_push, f) {#id, size, n_pop, n_push, OP_FMT_##f},
#else
#define DEF(id, size, n_pop, n_push, f) {size, n_pop, n_push, OP_FMT_##f},
#endif
#include "quickjs-opcode.h"
#undef DEF
#undef FMT
};

#define short_opcode_info(op)                                                  \
  opcode_info[(op) >= OP_TEMP_START ? (op) + (OP_TEMP_END - OP_TEMP_START)     \
                                    : (op)]

//
// Reading and loading source JSON data
//
char *read_file(const char *filename) {
  FILE *file = fopen(filename, "rb");
  if (file == NULL) {
    perror("File opening failed");
    return NULL;
  }

  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  rewind(file);

  char *data = (char *)malloc(length + 1);
  if (data == NULL) {
    fclose(file);
    return NULL;
  }

  fread(data, 1, length, file);
  data[length] = '\0'; // Null-terminate

  fclose(file);
  return data;
}

// For convenience

json load_json(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    // Handle file open error (throw exception or return empty JSON object)
    throw std::runtime_error("Could not open file: " + path);
  }

  // The library streams the file directly into the JSON object!
  return json::parse(file);
}

IridiumSEXP *parseIridiumSEXP(const json &node);

void populateArgs(IridiumSEXP *res, const json &args) {
  res->numArgs = args.size();
  // Switch to `new` instead of `malloc` for C++ object initialization
  res->args = new IridiumSEXP *[res->numArgs];

  int idx = 0;
  // Range-based for loop completely eliminates linked-list traversal!
  for (const auto &arg : args) {
    res->args[idx++] = parseIridiumSEXP(arg);
  }
}

void populateFlags(IridiumSEXP *res, const json &flags) {
  res->numFlags = flags.size();
  res->flags = new IridiumFlag *[res->numFlags];

  int idx = 0;
  for (const auto &flag : flags) {
    if (!flag.is_array() || flag.size() != 2) {
      fprintf(stderr, "Expected flag array size to be 2\n");
      exit(1);
    }

    IridiumFlag *currFlag = new IridiumFlag;
    res->flags[idx++] = currFlag;

    // Array access by index [0] and [1]
    currFlag->name = flag[0].get<std::string>();
    const auto &flagVal = flag[1];

    if (flagVal.is_boolean()) {
      currFlag->boolean_val = flagVal.get<bool>();
      currFlag->datatype = BOOLEAN;
    } else if (flagVal.is_number()) {
      currFlag->number_val = flagVal.get<double>();
      currFlag->datatype = NUMBER;
    } else if (flagVal.is_string()) {
      currFlag->string_val =
          flagVal.get<std::string>(); // Null-safe extraction!
      currFlag->datatype = STRING;
    } else if (flagVal.is_null()) {
      currFlag->datatype = NULLPTR;
    } else {
      fprintf(stderr, "Unsupported flag datatype\n");
      exit(1);
    }
  }
}

IridiumSEXP *parseNode(const std::string &tag, const json &args,
                       const json &flags) {
  IridiumSEXP *res = new IridiumSEXP;
  res->tag = tag;
  populateArgs(res, args);
  populateFlags(res, flags);
  return res;
}

IridiumSEXP *parseIridiumSEXP(const json &node) {
  if (!node.is_array()) {
    fprintf(stderr, "Iridium parsing failed, expected an array\n");
    exit(1);
  }

  // Ensure the array has the expected 3 elements: [tag, args, flags]
  if (node.size() < 3) {
    fprintf(stderr, "Iridium parsing failed, expected at least 3 elements\n");
    exit(1);
  }

  if (!node[0].is_string()) {
    fprintf(stderr, "Iridium parsing failed, expected tag to be a string\n");
    exit(1);
  }

  std::string tag = node[0].get<std::string>();
  const auto &args = node[1];
  const auto &flags = node[2];

  // Pre-Assertions
  if (!args.is_array()) {
    fprintf(stderr, "Expected args to be an array\n");
    exit(1);
  }

  if (!flags.is_array()) {
    fprintf(stderr, "Expected flags to be an array\n");
    exit(1);
  }

  // parse node
  return parseNode(tag, args, flags);
}

//
// Printing Iridium S-Expression Tree
//
void printSpace(FILE *target, int space) {
  while (space-- > 0)
    fprintf(target, " ");
}

void dumpFlag(FILE *target, IridiumFlag *flag) {
  if (flag->datatype == NUMBER) {
    fprintf(target, "%s => %f", flag->name.data(), flag->number_val);
  } else if (flag->datatype == STRING) {
    fprintf(target, "%s => %s", flag->name.data(), flag->string_val.data());
  } else if (flag->datatype == BOOLEAN) {
    fprintf(target, "%s => %s", flag->name.data(),
            flag->boolean_val ? "TRUE" : "FALSE");
  } else {
    fprintf(target, "%s", flag->name.data());
  }
}

void dumpIridiumSEXP(FILE *target, IridiumSEXP *node, int space) {
  printSpace(target, space);
  fprintf(target, "%s(", node->tag.data());
  IridiumFlag **flags = node->flags;

  for (int i = 0; i < node->numFlags; ++i) {
    IridiumFlag *currFlag = flags[i];
    dumpFlag(target, currFlag);
    if (i + 1 != node->numFlags)
      fprintf(target, ", ");
  }

  fprintf(target, ")\n");

  IridiumSEXP **args = node->args;
  for (int i = 0; i < node->numArgs; ++i) {
    IridiumSEXP *currArg = args[i];
    dumpIridiumSEXP(target, currArg, space + 2);
  }
}

//
// Generate Bytecode
//
typedef struct BCLList {
  uint8_t bc;
  bool lambdaPoolReference;
  bool hasPoolData;
  JSValue poolData;
  bool presolvedTarget;
  union {
    int8_t one;
    int16_t two;
    int32_t four;
  } data;
  uint8_t valueSize;
  bool hasFlags;
  uint8_t flags;
  // Extend the data structure to accomodate arguments dynamically
} BCInstruction;

enum GOTOCONTEXT {
  MULTI,

  put_loc0 = 1000,
  put_loc1 = 1001,
  put_loc2 = 1002,
  put_loc3 = 1003,

  put_arg0 = 2000,
  put_arg1 = 2001,
  put_arg2 = 2002,
  put_arg3 = 2003,

  put_var_ref0 = 3000,
  put_var_ref1 = 3001,
  put_var_ref2 = 3002,
  put_var_ref3 = 3003
};

struct GOTOINFO {
  GOTOCONTEXT kind;
  std::set<double> putTargets;

  GOTOINFO() : kind(GOTOCONTEXT::MULTI) {}

  GOTOINFO(vector<BCInstruction> &instructions) {
    auto &currInst = instructions.back();
    kind = getInstContext(currInst);

    if (kind != GOTOCONTEXT::MULTI)
      putTargets.insert(instructions.size() - 1);
  }

  void reduce(double idx) {
    if (kind == GOTOCONTEXT::MULTI)
      return;

    // Collect elements > idx
    std::vector<double> toUpdate;
    for (double val : putTargets) {
      if (val > idx)
        toUpdate.push_back(val);
    }

    // Remove old ones and insert reduced ones
    for (double val : toUpdate) {
      putTargets.erase(val);
      putTargets.insert(val - 1);
    }
  }

  static GOTOCONTEXT getInstContext(BCInstruction currInst) {
    if (currInst.bc == OP_put_loc0)
      return GOTOCONTEXT::put_loc0;
    if (currInst.bc == OP_put_loc1)
      return GOTOCONTEXT::put_loc1;
    if (currInst.bc == OP_put_loc2)
      return GOTOCONTEXT::put_loc2;
    if (currInst.bc == OP_put_loc3)
      return GOTOCONTEXT::put_loc3;

    if (currInst.bc == OP_put_arg0)
      return GOTOCONTEXT::put_arg0;
    if (currInst.bc == OP_put_arg1)
      return GOTOCONTEXT::put_arg1;
    if (currInst.bc == OP_put_arg2)
      return GOTOCONTEXT::put_arg2;
    if (currInst.bc == OP_put_arg3)
      return GOTOCONTEXT::put_arg3;

    if (currInst.bc == OP_put_var_ref0)
      return GOTOCONTEXT::put_var_ref0;
    if (currInst.bc == OP_put_var_ref1)
      return GOTOCONTEXT::put_var_ref1;
    if (currInst.bc == OP_put_var_ref2)
      return GOTOCONTEXT::put_var_ref2;
    if (currInst.bc == OP_put_var_ref3)
      return GOTOCONTEXT::put_var_ref3;

    return GOTOCONTEXT::MULTI;
  }

  void merge(vector<BCInstruction> &instructions) {
    if (kind == GOTOCONTEXT::MULTI)
      return;

    auto &currInst = instructions.back();
    if (getInstContext(currInst) != kind)
      kind = GOTOCONTEXT::MULTI;

    if (kind != GOTOCONTEXT::MULTI)
      putTargets.insert(instructions.size() - 1);
  }
};

std::unordered_map<double, GOTOINFO> *gotoContextMap = NULL;
std::unordered_map<uint32_t, size_t> *xyzz;

// ============== Push OP ============== //

void pushOP(JSContext *ctx, vector<BCInstruction> &instructions,
            OPCodeEnum opcode) {
  BCInstruction inst;
  inst.bc = opcode;
  inst.lambdaPoolReference = false;
  inst.hasPoolData = false;
  inst.poolData = JS_UNINITIALIZED;
  inst.presolvedTarget = false;
  inst.valueSize = 0;
  inst.data.four = 0;
  inst.hasFlags = false;
  inst.flags = 0;
  instructions.push_back(inst);
  return;
}

void pushOPFlags(JSContext *ctx, vector<BCInstruction> &instructions,
                 OPCodeEnum opcode, uint8_t flags) {
  BCInstruction inst;
  inst.bc = opcode;
  inst.lambdaPoolReference = false;
  inst.hasPoolData = false;
  inst.poolData = JS_UNINITIALIZED;
  inst.presolvedTarget = false;
  inst.valueSize = 0;
  inst.data.four = 0;
  inst.hasFlags = true;
  inst.flags = flags;
  instructions.push_back(inst);
  return;
}

void pushOP8(JSContext *ctx, vector<BCInstruction> &instructions,
             OPCodeEnum opcode, uint8_t data) {
  BCInstruction inst;
  inst.bc = opcode;
  inst.lambdaPoolReference = false;
  inst.hasPoolData = false;
  inst.poolData = JS_UNINITIALIZED;
  inst.presolvedTarget = false;
  inst.valueSize = 1;
  inst.data.one = data;
  inst.hasFlags = false;
  inst.flags = 0;
  inst.hasFlags = false;
  inst.flags = 0;
  instructions.push_back(inst);
  return;
}

void pushOP16(JSContext *ctx, vector<BCInstruction> &instructions,
              OPCodeEnum opcode, uint16_t data) {
  BCInstruction inst;
  inst.bc = opcode;
  inst.lambdaPoolReference = false;
  inst.hasPoolData = false;
  inst.poolData = JS_UNINITIALIZED;
  inst.presolvedTarget = false;
  inst.valueSize = 2;
  inst.data.two = data;
  inst.hasFlags = false;
  inst.flags = 0;
  instructions.push_back(inst);
  return;
}

void pushOP32(JSContext *ctx, vector<BCInstruction> &instructions,
              OPCodeEnum opcode, uint32_t data) {
  BCInstruction inst;
  inst.bc = opcode;
  inst.lambdaPoolReference = false;
  inst.hasPoolData = false;
  inst.poolData = JS_UNINITIALIZED;
  inst.presolvedTarget = false;
  inst.valueSize = 4;
  inst.data.four = data;
  inst.hasFlags = false;
  inst.flags = 0;
  instructions.push_back(inst);
  return;
}

void pushOP32Flags(JSContext *ctx, vector<BCInstruction> &instructions,
                   OPCodeEnum opcode, uint32_t data, uint8_t flags) {
  BCInstruction inst;
  inst.bc = opcode;
  inst.lambdaPoolReference = false;
  inst.hasPoolData = false;
  inst.poolData = JS_UNINITIALIZED;
  inst.presolvedTarget = false;
  inst.valueSize = 4;
  inst.data.four = data;
  inst.hasFlags = true;
  inst.flags = flags;
  instructions.push_back(inst);
  return;
}

void pushOPConst(JSContext *ctx, vector<BCInstruction> &instructions,
                 OPCodeEnum opcode, JSValue cData) {
  BCInstruction inst;
  inst.bc = opcode;
  inst.lambdaPoolReference = false;
  inst.hasPoolData = true;
  inst.poolData = cData;
  inst.presolvedTarget = false;
  inst.valueSize = 4;
  inst.data.four = 0;
  inst.hasFlags = false;
  inst.flags = 0;
  instructions.push_back(inst);
  return;
}

// ============== Push OP ============== //

// ============== Flag Related ============== //

bool isTag(IridiumSEXP *node, const std::string &tag) {
  return node->tag == tag;
}

void ensureTag(IridiumSEXP *node, const std::string &tag) {
  if (!isTag(node, tag)) {
    // std::cerr handles std::string natively, unlike fprintf
    std::cerr << "Expected tag " << tag << ", found " << node->tag << "\n";
    exit(1);
  }
}

bool hasFlag(IridiumSEXP *node, const std::string &flagToCheck) {
  for (int i = 0; i < node->numFlags; i++) {
    IridiumFlag *flag = node->flags[i];
    if (flag->name == flagToCheck) // No more strcmp!
      return true;
  }
  return false;
}

void ensureFlag(IridiumSEXP *node, const std::string &flag) {
  if (!hasFlag(node, flag)) {
    std::cerr << "Expected flag " << flag << " not found\n";
    exit(1);
  }
}

IridiumFlag *getFlag(IridiumSEXP *node, const std::string &flagToCheck) {
  for (int i = 0; i < node->numFlags; i++) {
    IridiumFlag *flag = node->flags[i];
    if (flag->name == flagToCheck)
      return flag;
  }
  std::cerr << "Failed to get flag, " << flagToCheck << " not found\n";
  throw std::runtime_error("Failed to get flag, " + flagToCheck +
                           " not found\n");
  exit(1);
}

int getFlagNumber(IridiumSEXP *binding, const std::string &flagName) {
  IridiumFlag *flag = getFlag(binding, flagName);
  if (flag->datatype == NUMBER) {
    return static_cast<int>(flag->number_val); // Cast double to int
  }
  std::cerr << "TODO: failed to get NUMBER\n";
  exit(1);
}

double getFlagDouble(IridiumSEXP *binding, const std::string &flagName) {
  IridiumFlag *flag = getFlag(binding, flagName);
  if (flag->datatype == NUMBER) {
    return flag->number_val;
  }
  std::cerr << "TODO: failed to get NUMBER(double)\n";
  exit(1);
}

// ---------------------------------------------------------
// THE OLD API (Legacy)
// Returns char* so your existing code compiles without changes.
// ---------------------------------------------------------
char *getFlagString(IridiumSEXP *binding, const char *flagName) {
  // C++ implicitly converts the const char* to a std::string for getFlag
  IridiumFlag *flag = getFlag(binding, flagName);

  if (flag->datatype == STRING) {
    // In C++17 and later, .data() returns a non-const char*
    // pointing to the internal string buffer.
    return flag->string_val.data();
  }
  std::cerr << "TODO: failed to get STRING\n";
  exit(1);
}

// ---------------------------------------------------------
// THE NEW API (Modern)
// Returns a const reference to the full std::string.
// Use this for RegExp and strings that might contain \0.
// ---------------------------------------------------------
const std::string &getFlagStdString(IridiumSEXP *binding,
                                    const std::string &flagName) {
  IridiumFlag *flag = getFlag(binding, flagName);

  if (flag->datatype == STRING) {
    return flag->string_val;
  }
  std::cerr << "TODO: failed to get STRING\n";
  exit(1);
}

bool getFlagBoolean(IridiumSEXP *binding, const std::string &flagName) {
  IridiumFlag *flag = getFlag(binding, flagName);
  if (flag->datatype == BOOLEAN) {
    return flag->boolean_val;
  }
  std::cerr << "TODO: failed to get BOOLEAN\n";
  exit(1);
}

int getFlagNull(IridiumSEXP *binding, const std::string &flagName) {
  IridiumFlag *flag = getFlag(binding, flagName);
  if (flag->datatype == NULLPTR) {
    return 0;
  }
  std::cerr << "TODO: failed to get NULLPTR\n";
  exit(1);
}
// ============== Flag Related ============== //

// ============== Code Generation ============== //

int parse_arg_index(const char *str) {
  const char *prefix = "ARG";
  size_t prefix_len = strlen(prefix);

  if (strncmp(str, prefix, prefix_len) != 0)
    return -1;

  // Ensure remaining characters are digits
  const char *number_part = str + prefix_len;
  for (const char *p = number_part; *p != '\0'; ++p) {
    if (!isdigit((unsigned char)*p))
      return -1;
  }

  return atoi(number_part);
}

// ============== Misc ============== //

void storeWhatevesOnTheStack(JSContext *ctx, IridiumSEXP *loc,
                             vector<BCInstruction> &instructions, bool safe,
                             bool thisInit, bool isStrict,
                             bool saveResToStack) {
  if (saveResToStack)
    pushOP(ctx, instructions, OP_dup);

  // is lval global?
  if (isTag(loc, "GlobalBinding")) {
    char *name = getFlagString(loc, "NAME");

    if (safe) {
      pushOP32(ctx, instructions, OP_put_var_init, JS_NewAtom(ctx, name));
    } else if (isStrict) {
      pushOP32(ctx, instructions, OP_check_var, JS_NewAtom(ctx, name));
      pushOP(ctx, instructions, OP_swap);
      pushOP32(ctx, instructions, OP_put_var_strict, JS_NewAtom(ctx, name));
    } else {
      pushOP32(ctx, instructions, OP_put_var, JS_NewAtom(ctx, name));
    }
  } else {
    if (isTag(loc, "RemoteEnvBinding")) {

      if (!safe) {
        // Unsafe writes const results in error...
        IridiumSEXP *next = loc->args[0];

        while (isTag(next, "RemoteEnvBinding")) {
          next = next->args[0];
        }

        ensureTag(next, "EnvBinding");

        if (hasFlag(next, "JSCONST") && !thisInit) {
          pushOP(ctx, instructions, OP_drop);
          pushOP32Flags(ctx, instructions, OP_throw_error,
                        JS_NewAtom(ctx, getFlagString(next, "NAME")), 0);
          return;
        }
      }

      int refIDX = getFlagNumber(loc, "REFIDX");

      // === SPECIALIZATION ===
      if (thisInit)
        return pushOP16(ctx, instructions, OP_put_var_ref_check_init, refIDX);

      if (!safe)
        return pushOP16(ctx, instructions, OP_put_var_ref_check, refIDX);

      switch (refIDX) {
      case 0:
        return pushOP(ctx, instructions, OP_put_var_ref0);
        break;
      case 1:
        return pushOP(ctx, instructions, OP_put_var_ref1);
        break;
      case 2:
        return pushOP(ctx, instructions, OP_put_var_ref2);
        break;
      case 3:
        return pushOP(ctx, instructions, OP_put_var_ref3);
        break;
      }

      return pushOP16(ctx, instructions, OP_put_var_ref, refIDX);
    } else if (isTag(loc, "EnvBinding")) {

      if (hasFlag(loc, "JSARG")) {
        int argIdx = getFlagNumber(loc, "REFIDX");
        assert(argIdx > -1);

        // === SPECIALIZATION ===
        switch (argIdx) {
        case 0:
          return pushOP(ctx, instructions, OP_put_arg0);
          break;
        case 1:
          return pushOP(ctx, instructions, OP_put_arg1);
          break;
        case 2:
          return pushOP(ctx, instructions, OP_put_arg2);
          break;
        case 3:
          return pushOP(ctx, instructions, OP_put_arg3);
          break;
        default:
          return pushOP16(ctx, instructions, OP_put_arg, argIdx);
        }
      } else if (hasFlag(loc, "JSRESTARG")) {
        fprintf(stderr, "TODO: handle stores to JSRESTARG");
        exit(1);
      }

      if (!safe) {
        if (hasFlag(loc, "JSCONST") && !thisInit) {
          pushOP(ctx, instructions, OP_drop);
          pushOP32Flags(ctx, instructions, OP_throw_error,
                        JS_NewAtom(ctx, getFlagString(loc, "NAME")), 0);
          return;
        }
      }

      int refIdx = getFlagNumber(loc, "REFIDX");
      if (thisInit) {
        pushOP16(ctx, instructions, OP_put_loc_check_init, refIdx);
      } else if (safe) {
        // === SPECIALIZATION ===
        switch (refIdx) {
        case 0:
          pushOP(ctx, instructions, OP_put_loc0);
          break;
        case 1:
          pushOP(ctx, instructions, OP_put_loc1);
          break;
        case 2:
          pushOP(ctx, instructions, OP_put_loc2);
          break;
        case 3:
          pushOP(ctx, instructions, OP_put_loc3);
          break;
        default: {
          if (refIdx < 256) {
            pushOP8(ctx, instructions, OP_put_loc8, refIdx);
          } else {
            pushOP16(ctx, instructions, OP_put_loc, refIdx);
          }
          break;
        }
        }
      } else {
        pushOP16(ctx, instructions, OP_put_loc_check, refIdx);
      }
    } else {
      fprintf(stderr, "TODO: store on stack, unhandled loc kind!!");
      exit(1);
    }
  }
  return;
}

// Gemini 2.5 pro, got it right, maybe...
void keepNDropM(JSContext *ctx, vector<BCInstruction> &instructions, int N,
                int M) {
  // If M is zero or negative, no elements need to be dropped.
  if (M <= 0)
    return;

  // If N is negative, it's an invalid input. Do nothing.
  if (N < 0) {
    fprintf(stderr, "TODO: keepMDropN, invalid input !!");
    exit(1);
  }

  if (N == 2 && M == 1) {
    pushOP(ctx, instructions, OP_nip1);
    return;
  } else if (N == 1 && M == 2) {
    pushOP(ctx, instructions, OP_nip);
    pushOP(ctx, instructions, OP_nip);
    return;
  }
  fprintf(stderr, "TODO: keepMDropN, handle general case, validate code!!");
  exit(1);

  return;
}

void lowerToStack(JSContext *ctx, vector<BCInstruction> &instructions,
                  IridiumSEXP *rval, bool safeRead = false);

void handleFieldRead(JSContext *ctx, vector<BCInstruction> &instructions,
                     IridiumSEXP *rval, bool retainContext = false) {
  IridiumSEXP *receiver = rval->args[0];
  lowerToStack(ctx, instructions, receiver);
  IridiumSEXP *field = rval->args[1];
  ensureTag(field, "String");
  auto fieldNameString = getFlagString(field, "IridiumPrimitive");
  if (!retainContext && std::string(fieldNameString) == "length") {
    pushOP(ctx, instructions, OP_get_length);
  } else {
    JSAtom fieldAtom = JS_NewAtom(ctx, fieldNameString);
    pushOP32(ctx, instructions, retainContext ? OP_get_field2 : OP_get_field,
             fieldAtom);
  }
}

void handleFieldWrite(JSContext *ctx, vector<BCInstruction> &instructions,
                      IridiumSEXP *rval, bool retainContext = false) {
  // Receiver
  IridiumSEXP *receiver = rval->args[0];
  lowerToStack(ctx, instructions, receiver);

  // Rval
  IridiumSEXP *valToPush = rval->args[2];
  lowerToStack(ctx, instructions, valToPush);

  if (retainContext) {
    // rcvr rval -> rval rcvr rval
    pushOP(ctx, instructions, OP_insert2);
  }

  // Field
  IridiumSEXP *field = rval->args[1];
  ensureTag(field, "String");
  JSAtom fieldAtom = JS_NewAtom(ctx, getFlagString(field, "IridiumPrimitive"));
  pushOP32(ctx, instructions, OP_put_field, fieldAtom);
}

void handleComputedFieldRead(JSContext *ctx,
                             vector<BCInstruction> &instructions,
                             IridiumSEXP *rval, bool retainContext = false) {
  IridiumSEXP *receiver = rval->args[0];
  lowerToStack(ctx, instructions, receiver);
  IridiumSEXP *field = rval->args[1];
  lowerToStack(ctx, instructions, field);
  return pushOP(ctx, instructions,
                retainContext ? OP_get_array_el2 : OP_get_array_el);
}

// Opt this...
void handleJSPrivateFieldWrite(JSContext *ctx,
                               vector<BCInstruction> &instructions,
                               IridiumSEXP *rval) {
  bool isDeclaration = hasFlag(rval, "DECL");
  if (isDeclaration) {
    lowerToStack(ctx, instructions, rval->args[0]);     // obj
    lowerToStack(ctx, instructions, rval->args[1]);     // obj prop
    lowerToStack(ctx, instructions, rval->args[2]);     // obj prop value
    pushOP(ctx, instructions, OP_insert3);              // value obj prop value
    pushOP(ctx, instructions, OP_define_private_field); // value obj
    pushOP(ctx, instructions, OP_drop);                 // value
  } else {
    lowerToStack(ctx, instructions, rval->args[0]);  // obj
    lowerToStack(ctx, instructions, rval->args[2]);  // obj value
    pushOP(ctx, instructions, OP_insert2);           // value obj value
    lowerToStack(ctx, instructions, rval->args[1]);  // value obj value prop
    pushOP(ctx, instructions, OP_put_private_field); // value
  }
}

void handleComputedFieldWrite(JSContext *ctx,
                              vector<BCInstruction> &instructions,
                              IridiumSEXP *rval, bool retainContext = false) {
  // Receiver
  IridiumSEXP *receiver = rval->args[0];
  lowerToStack(ctx, instructions, receiver);

  // Field
  IridiumSEXP *field = rval->args[1];
  lowerToStack(ctx, instructions, field);
  if (!hasFlag(rval, "SAFE"))
    pushOP(ctx, instructions, OP_to_propkey);

  // Rval
  IridiumSEXP *assnVal = rval->args[2];
  lowerToStack(ctx, instructions, assnVal);

  if (retainContext) {
    // Receiver Field Rval -> Rval Receiver Field Rval
    pushOP(ctx, instructions, OP_insert3);
  }

  pushOP(ctx, instructions, OP_put_array_el); // Receiver Field Rval
}

void handleCallSite(JSContext *ctx, vector<BCInstruction> &instructions,
                    IridiumSEXP *rval) {
  int i = 0;

  // Handle Constructor Call Context, the class object is duplicated on the
  // stack
  if (hasFlag(rval, "ConstructorCall")) {
    assert(rval->numArgs >= 1);
    lowerToStack(ctx, instructions, rval->args[0]);
    pushOP(ctx, instructions, OP_dup);
    i = 1;
  }

  // Private Call needs to be behind a brand check
  if (hasFlag(rval, "PrivateCall")) {
    assert(rval->numArgs >= 2);
    lowerToStack(ctx, instructions, rval->args[0]);
    pushOP(ctx, instructions, OP_dup);
    lowerToStack(ctx, instructions, rval->args[1]);
    i = 2;
  }

  // Peephole optimization for fields of call statements...
  if (hasFlag(rval, "CCall")) {
    // If the context object and the receiver object are the same, we can
    // simply ignore the first argument and retain the context upon field
    // lookup
    auto contextObj = rval->args[0];
    auto lookup = rval->args[1];

    if (isTag(lookup, "FieldRead") || isTag(lookup, "JSComputedFieldRead")) {
      if (isTag(lookup->args[0], "EnvRead")) {
        auto receiver = lookup->args[0]->args[0];
        if (isTag(receiver, "GlobalBinding")) {
          if (isTag(contextObj, "EnvRead")) {
            auto bindingReadByContextObj = contextObj->args[0];
            if (isTag(bindingReadByContextObj, "GlobalBinding")) {
              if (strcmp(getFlagString(receiver, "NAME"),
                         getFlagString(bindingReadByContextObj, "NAME")) == 0) {
                // handleFieldRead(ctx, instructions, lookup, true);
                if (isTag(lookup, "FieldRead")) {
                  handleFieldRead(ctx, instructions, lookup, true);
                } else if (isTag(lookup, "JSComputedFieldRead")) {
                  handleComputedFieldRead(ctx, instructions, lookup, true);
                }
                i = 2;
              }
            }
          }
        } else if (isTag(receiver, "EnvBinding")) {
          int receiverIDX = getFlagNumber(receiver, "REFIDX");
          if (isTag(contextObj, "EnvRead")) {
            auto bindingReadByContextObj = contextObj->args[0];
            if (isTag(bindingReadByContextObj, "EnvBinding")) {
              int contextIDX = getFlagNumber(bindingReadByContextObj, "REFIDX");
              if (receiverIDX == contextIDX) {
                // handleFieldRead(ctx, instructions, lookup, true);
                if (isTag(lookup, "FieldRead")) {
                  handleFieldRead(ctx, instructions, lookup, true);
                } else if (isTag(lookup, "JSComputedFieldRead")) {
                  handleComputedFieldRead(ctx, instructions, lookup, true);
                }
                i = 2;
              }
            }
          }
        } else if (isTag(receiver, "RemoteEnvBinding")) {
          int receiverIDX = getFlagNumber(receiver, "REFIDX");
          if (isTag(contextObj, "EnvRead")) {
            auto bindingReadByContextObj = contextObj->args[0];
            if (isTag(bindingReadByContextObj, "RemoteEnvBinding")) {
              int contextIDX = getFlagNumber(bindingReadByContextObj, "REFIDX");
              if (receiverIDX == contextIDX) {
                // handleFieldRead(ctx, instructions, lookup, true);
                if (isTag(lookup, "FieldRead")) {
                  handleFieldRead(ctx, instructions, lookup, true);
                } else if (isTag(lookup, "JSComputedFieldRead")) {
                  handleComputedFieldRead(ctx, instructions, lookup, true);
                }
                i = 2;
              }
            }
          }
        }
      }
    } else {
      // Todo handle more cases
    }
  }

  bool isTailCall = hasFlag(rval, "TAILCALL");

  // Lower Arguments
  for (; i < rval->numArgs; i++) {
    lowerToStack(ctx, instructions, rval->args[i]);
  }

  if (hasFlag(rval, "JSDirectEval")) {
    uint32_t data = 0;
    uint16_t *d1 = (uint16_t *)&data;
    uint16_t *d2 = d1 + 1;

    *d1 = rval->numArgs - 1;
    *d2 = getFlagNumber(rval, "JSDirectEval"); // scopeIdx

    // Arguments were pushed
    return pushOP32(ctx, instructions, OP_eval, data);
  } else if (hasFlag(rval, "Super")) {
    return pushOP16(ctx, instructions, OP_call_constructor, rval->numArgs - 2);
  } else if (hasFlag(rval, "ConstructorCall")) {
    return pushOP16(ctx, instructions, OP_call_constructor, rval->numArgs - 1);
  } else if (hasFlag(rval, "CCall")) {
    if (isTailCall)
      return pushOP16(ctx, instructions, OP_tail_call_method,
                      rval->numArgs - 2);
    return pushOP16(ctx, instructions, OP_call_method, rval->numArgs - 2);
  } else if (hasFlag(rval, "PrivateCall")) {
    if (isTailCall)
      return pushOP16(ctx, instructions, OP_tail_call_method,
                      rval->numArgs - 2);
    return pushOP16(ctx, instructions, OP_call_method, rval->numArgs - 2);
  } else {
    auto fArgs = rval->numArgs - 1;
    if (isTailCall)
      return pushOP16(ctx, instructions, OP_tail_call, fArgs);
    switch (fArgs) {
    case 0:
      return pushOP(ctx, instructions, OP_call0);
      break;
    case 1:
      return pushOP(ctx, instructions, OP_call1);
      break;
    case 2:
      return pushOP(ctx, instructions, OP_call2);
      break;
    case 3:
      return pushOP(ctx, instructions, OP_call3);
      break;
    default:
      return pushOP16(ctx, instructions, OP_call, fArgs);
      break;
    }
  }
}

void handleApply(JSContext *ctx, vector<BCInstruction> &instructions,
                 IridiumSEXP *rval) {
  assert(rval->numArgs == 3 && "Apply requires exactly three arguments");

  auto &callee = rval->args[0];
  auto &calleeCTX = rval->args[1];
  auto &argList = rval->args[2];
  bool isConstructorCall = hasFlag(rval, "ConstructorCall");
  bool isJSDirectEval = hasFlag(rval, "JSDirectEval");
  bool isSuper = hasFlag(rval, "Super");

  // Lower callee
  lowerToStack(ctx, instructions, callee);

  if (isConstructorCall && isJSDirectEval) {
    throw std::runtime_error("tried to call eval as a constructor");
  }

  // Lower callee context
  if (isSuper) {
    lowerToStack(ctx, instructions, calleeCTX);
  } else if (isConstructorCall) {
    pushOP(ctx, instructions, OP_dup);
  } else if (isJSDirectEval) {
    // Do nothing
  } else {
    lowerToStack(ctx, instructions, calleeCTX);
  }

  // Lower arglist
  lowerToStack(ctx, instructions, argList);

  // Emit call
  if (isSuper) {
    return pushOP16(ctx, instructions, OP_apply, 1);
  } else if (isConstructorCall) {
    return pushOP16(ctx, instructions, OP_apply, 1);
  } else if (isJSDirectEval) {
    double directEvalScope = getFlagDouble(rval, "JSDirectEval");
    return pushOP16(ctx, instructions, OP_apply_eval, directEvalScope);
  } else {
    return pushOP16(ctx, instructions, OP_apply, 0);
  }
}

void lowerToStack(JSContext *ctx, vector<BCInstruction> &instructions,
                  IridiumSEXP *rval, bool safeRead) {
  if (isTag(rval, "JSCTX")) {
    int OPID = getFlagNumber(rval, "OPID");
    switch (OPID) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6: {
      return pushOP8(ctx, instructions, OP_special_object, OPID);
    }
    case 7:
    case 8: {
      assert(rval->numArgs == 1);
      lowerToStack(ctx, instructions, rval->args[0]);
      return pushOP(ctx, instructions, OP_get_super);
    }
    case 9: {
      return pushOP(ctx, instructions, OP_push_this);
    }
    default:
      fprintf(stderr, "TODO: OPID: %d for JSCTX\n", OPID);
      exit(1);
      break;
    }
  } else if (isTag(rval, "Yield")) {
    lowerToStack(ctx, instructions, rval->args[0]);
    pushOP(ctx, instructions, OP_yield);
  } else if (isTag(rval, "JSForInStart")) {
    // Push obj onto the stack
    lowerToStack(ctx, instructions, rval->args[0]);

    // obj -> enum_obj
    return pushOP(ctx, instructions, OP_for_in_start);
  } else if (isTag(rval, "JSToObject")) {
    lowerToStack(ctx, instructions, rval->args[0]);
    return pushOP(ctx, instructions, OP_to_object);
  } else if (isTag(rval, "String")) {
    // 1. Safely extract the full C++ string, preserving any \0 bytes
    std::string strVal = getFlagStdString(rval, "IridiumPrimitive");

    // 2. SPECIALIZATION: Use the native .empty() method.
    // This will correctly return false for "\0②" because its size is 4, not 0!
    if (strVal.empty()) {
      return pushOP(ctx, instructions, OP_push_empty_string);
    }

    // 3. Create the atom using the explicit length to bypass QuickJS truncation
    JSAtom strAtom = JS_NewAtomLen(ctx, strVal.data(), strVal.size());
    return pushOP32(ctx, instructions, OP_push_atom_value, strAtom);
  } else if (isTag(rval, "RegExp")) {
    std::string exp = getFlagStdString(rval, "EXP");
    std::string flags = getFlagStdString(rval, "FLAGS");

    JSValue expValue = JS_NewStringLen(ctx, exp.data(), exp.size());
    JSValue flagsValue = JS_NewStringLen(ctx, flags.data(), flags.size());

    pushOPConst(ctx, instructions, OP_push_const, expValue);

    if (!ctx->compile_regexp) {
      fprintf(stderr, "RegExp compiler not found in the context\n");
      exit(1);
    }
    JSValue compiledRegexp = ctx->compile_regexp(ctx, expValue, flagsValue);
    pushOPConst(ctx, instructions, OP_push_const, compiledRegexp);
    return pushOP(ctx, instructions, OP_regexp);
  } else if (isTag(rval, "JSTemplate")) {
    pushOP(ctx, instructions, OP_push_empty_string);
    JSAtom fieldAtom = JS_NewAtom(ctx, "concat");
    pushOP32(ctx, instructions, OP_get_field2, fieldAtom);

    for (int i = 0; i < rval->numArgs; i++) {
      lowerToStack(ctx, instructions, rval->args[i]);
    }

    return pushOP16(ctx, instructions, OP_call_method, rval->numArgs);
  } else if (isTag(rval, "Number")) {
    double data = getFlagDouble(rval, "IridiumPrimitive");

    // Check if exactly an integer
    double truncated = trunc(data);
    if (data == truncated) {
      int64_t val = (int64_t)truncated;

      switch (val) {
      case -1:
        return pushOP(ctx, instructions, OP_push_minus1);
      case 0:
        return pushOP(ctx, instructions, OP_push_0);
      case 1:
        return pushOP(ctx, instructions, OP_push_1);
      case 2:
        return pushOP(ctx, instructions, OP_push_2);
      case 3:
        return pushOP(ctx, instructions, OP_push_3);
      case 4:
        return pushOP(ctx, instructions, OP_push_4);
      case 5:
        return pushOP(ctx, instructions, OP_push_5);
      case 6:
        return pushOP(ctx, instructions, OP_push_6);
      case 7:
        return pushOP(ctx, instructions, OP_push_7);
      }

      if (val >= INT8_MIN && val <= INT8_MAX)
        return pushOP8(ctx, instructions, OP_push_i8, (int8_t)val);

      if (val >= INT16_MIN && val <= INT16_MAX)
        return pushOP16(ctx, instructions, OP_push_i16, (int16_t)val);

      if (val >= INT32_MIN && val <= INT32_MAX)
        return pushOP32(ctx, instructions, OP_push_i32, (int32_t)val);
    }

    // Fallback
    JSValue jsvalue = JS_NewNumber(ctx, data);
    return pushOPConst(ctx, instructions, OP_push_const, jsvalue);
  } else if (isTag(rval, "JSNUBD")) {
    return pushOPConst(ctx, instructions, OP_push_const, JS_UNINITIALIZED);
  } else if (isTag(rval, "RemoteEnvBinding")) {
    int refIDX = getFlagNumber(rval, "REFIDX");

    // === SPECIALIZATION ===
    if (!safeRead)
      return pushOP16(ctx, instructions, OP_get_var_ref_check, refIDX);
    switch (refIDX) {
    case 0:
      return pushOP(ctx, instructions, OP_get_var_ref0);
      break;
    case 1:
      return pushOP(ctx, instructions, OP_get_var_ref1);
      break;
    case 2:
      return pushOP(ctx, instructions, OP_get_var_ref2);
      break;
    case 3:
      return pushOP(ctx, instructions, OP_get_var_ref3);
      break;
    }

    return pushOP16(ctx, instructions, OP_get_var_ref, refIDX);
  } else if (isTag(rval, "EnvBinding")) {
    // If this is an arg, get the arg idx
    if (hasFlag(rval, "JSARG")) {
      int argIdx = getFlagNumber(rval, "REFIDX");
      // int argIdx = parse_arg_index(getFlagString(rval->args[0],
      // "IridiumPrimitive"));
      assert(argIdx > -1);

      // === SPECIALIZATION ===
      switch (argIdx) {
      case 0:
        return pushOP(ctx, instructions, OP_get_arg0);
        break;
      case 1:
        return pushOP(ctx, instructions, OP_get_arg1);
        break;
      case 2:
        return pushOP(ctx, instructions, OP_get_arg2);
        break;
      case 3:
        return pushOP(ctx, instructions, OP_get_arg3);
        break;
      default:
        return pushOP16(ctx, instructions, OP_get_arg, argIdx);
      }
    } else if (hasFlag(rval, "JSRESTARG")) {
      int argIdx = getFlagNumber(rval, "REFIDX");
      // int argIdx = parse_arg_index(getFlagString(rval->args[0],
      // "IridiumPrimitive"));
      assert(argIdx > -1);
      pushOP16(ctx, instructions, OP_rest, argIdx);
    } else {
      int refIDX = getFlagNumber(rval, "REFIDX");

      // === SPECIALIZATION ===
      if (!safeRead)
        return pushOP16(ctx, instructions, OP_get_loc_check, refIDX);
      switch (refIDX) {
      case 0:
        return pushOP(ctx, instructions, OP_get_loc0);
        break;
      case 1:
        return pushOP(ctx, instructions, OP_get_loc1);
        break;
      case 2:
        return pushOP(ctx, instructions, OP_get_loc2);
        break;
      case 3:
        return pushOP(ctx, instructions, OP_get_loc3);
        break;

      default: {
        if (refIDX < 256) {
          return pushOP8(ctx, instructions, OP_get_loc8, refIDX);
        } else {
          return pushOP16(ctx, instructions, OP_get_loc, refIDX);
        }
      }
      }
    }
  } else if (isTag(rval, "GlobalBinding")) {
    char *lookupVal = getFlagString(rval, "NAME");
    if (strcmp(lookupVal, "undefined") == 0)
      return pushOP(ctx, instructions, OP_undefined);
    else
      return pushOP32(ctx, instructions, OP_get_var,
                      JS_NewAtom(ctx, lookupVal));
  } else if (isTag(rval, "Apply")) {
    handleApply(ctx, instructions, rval);
  } else if (isTag(rval, "CallSite")) {
    handleCallSite(ctx, instructions, rval);
  } else if (isTag(rval, "EnvRead")) {
    return lowerToStack(ctx, instructions, rval->args[0],
                        hasFlag(rval, "SAFE"));
  } else if (isTag(rval, "PVTEnvRead")) {
    // This insturuction expects that the "this" context was passed by the
    // parent...
    lowerToStack(ctx, instructions, rval->args[0]);
    if (!hasFlag(rval, "FULLY_RESOLVE"))
      return;
    bool isMethod = hasFlag(rval, "METHOD");
    if (isMethod) {
      pushOP(ctx, instructions, OP_check_brand);
      pushOP(ctx, instructions, OP_nip);
    } else {
      pushOP(ctx, instructions, OP_get_private_field);
    }
    return;
  } else if (isTag(rval, "Boolean")) {
    bool res = getFlagBoolean(rval, "IridiumPrimitive");
    if (res) {
      return pushOP(ctx, instructions, OP_push_true);
    } else {
      return pushOP(ctx, instructions, OP_push_false);
    }
  } else if (isTag(rval, "Unop") || isTag(rval, "JSUnop")) {
    char *op = getFlagString(rval, "OP");
    if (strcmp(op, "!") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      return pushOP(ctx, instructions, OP_lnot);
    } else if (strcmp(op, "-") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      return pushOP(ctx, instructions, OP_neg);
    } else if (strcmp(op, "+") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      return pushOP(ctx, instructions, OP_plus);
    } else if (strcmp(op, "~") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      return pushOP(ctx, instructions, OP_not);
    } else if (strcmp(op, "typeof") == 0) {
      if (isTag(rval->args[0], "EnvRead") &&
          isTag(rval->args[0]->args[0], "GlobalBinding")) {
        // If the argument is an envread; then dont read the binding, use the
        // binding directly
        pushOP32(
            ctx, instructions, OP_get_var_undef,
            JS_NewAtom(ctx, getFlagString(rval->args[0]->args[0], "NAME")));
      } else {
        lowerToStack(ctx, instructions, rval->args[0]);
      }

      return pushOP(ctx, instructions, OP_typeof);
    } else if (strcmp(op, "delete") == 0) {
      IridiumSEXP *valToDelete = rval->args[0];
      if (isTag(valToDelete, "UNOPDelMemberExpr")) {
        IridiumSEXP *receiver = valToDelete->args[0];
        IridiumSEXP *field = valToDelete->args[1];
        lowerToStack(ctx, instructions, receiver);
        lowerToStack(ctx, instructions, field);
        return pushOP(ctx, instructions, OP_delete);
      } else if (isTag(valToDelete, "UNOPDelVar")) {
        JSAtom varAtom = JS_NewAtom(ctx, getFlagString(valToDelete, "NAME"));
        return pushOP32(ctx, instructions, OP_delete_var, varAtom);
      } else {
        fprintf(stderr, "TODO: Unhandled : %s, %s, %s\n", op, rval->tag.data(),
                valToDelete->tag.data());
        exit(1);
      }
    } else if (strcmp(op, "++") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      return pushOP(ctx, instructions, OP_inc);
    } else if (strcmp(op, "--") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      return pushOP(ctx, instructions, OP_dec);
    } else {
      fprintf(stderr, "TODO: unhandled Unop: %s\n", op);
      exit(1);
    }
  } else if (isTag(rval, "ToNumeric")) {
    lowerToStack(ctx, instructions, rval->args[0]);
    pushOP(ctx, instructions, OP_post_inc);
    return pushOP(ctx, instructions, OP_drop);
  } else if (isTag(rval, "Binop") || isTag(rval, "JSBinop")) {
    char *op = getFlagString(rval, "OP");
    if (strcmp(op, "+") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_add);
    } else if (strcmp(op, "-") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_sub);
    } else if (strcmp(op, "/") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_div);
    } else if (strcmp(op, "%") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_mod);
    } else if (strcmp(op, "*") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_mul);
    } else if (strcmp(op, "**") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_pow);
    } else if (strcmp(op, "&") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_and);
    } else if (strcmp(op, "|") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_or);
    } else if (strcmp(op, ">>") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_sar);
    } else if (strcmp(op, ">>>") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_shr);
    } else if (strcmp(op, "<<") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_shl);
    } else if (strcmp(op, "^") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_xor);
    } else if (strcmp(op, "==") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_eq);
    } else if (strcmp(op, "===") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_strict_eq);
    } else if (strcmp(op, "!=") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_neq);
    } else if (strcmp(op, "!==") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_strict_neq);
    } else if (strcmp(op, "pin") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_private_in);
    } else if (strcmp(op, "in") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_in);
    } else if (strcmp(op, "instanceof") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_instanceof);
    } else if (strcmp(op, ">") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_gt);
    } else if (strcmp(op, "<") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_lt);
    } else if (strcmp(op, ">=") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_gte);
    } else if (strcmp(op, "<=") == 0) {
      lowerToStack(ctx, instructions, rval->args[0]);
      lowerToStack(ctx, instructions, rval->args[1]);
      return pushOP(ctx, instructions, OP_lte);
    } else {
      fprintf(stderr, "TODO: binop %s\n", op);
      exit(1);
    }
  } else if (isTag(rval, "FieldRead")) {
    return handleFieldRead(ctx, instructions, rval, false);
  } else if (isTag(rval, "JSComputedFieldRead")) {
    return handleComputedFieldRead(ctx, instructions, rval, false);
  } else if (isTag(rval, "IDOP")) {
    bool isPrefix = getFlagBoolean(rval, "PREFIX");
    bool isIncrement = getFlagBoolean(rval, "INCREMENT");

    // Read field
    IridiumSEXP *fieldRead = rval->args[0];
    assert(isTag(fieldRead, "FieldRead"));
    IridiumSEXP *field = fieldRead->args[1];
    ensureTag(field, "String");
    JSAtom fieldAtom =
        JS_NewAtom(ctx, getFlagString(field, "IridiumPrimitive"));

    handleFieldRead(ctx, instructions, fieldRead, true);
    // obj obj.field

    if (isPrefix) {
      // Pre increment
      // obj oldVal
      pushOP(ctx, instructions, isIncrement ? OP_inc : OP_dec);
      // obj newVal
      pushOP(ctx, instructions, OP_insert2);
      // newVal obj newVal -- same as dup_x1 in Java bytecode
      pushOP32(ctx, instructions, OP_put_field, fieldAtom);
      // newVal
    } else {
      // Post increment
      // obj oldVal
      pushOP(ctx, instructions, isIncrement ? OP_post_inc : OP_post_dec);
      // obj oldVal newVal
      pushOP(ctx, instructions, OP_perm3);
      // oldVal obj newVal
      pushOP32(ctx, instructions, OP_put_field, fieldAtom);
      // oldVal
    }
  } else if (isTag(rval, "JSIDOP")) {
    bool isPrefix = getFlagBoolean(rval, "PREFIX");
    bool isIncrement = getFlagBoolean(rval, "INCREMENT");

    IridiumSEXP *read = rval->args[0];

    if (isTag(read, "JSComputedFieldRead")) {
      // ---- obj[key]++ / ++obj[key] : lvalue depth = 2 ----
      IridiumSEXP *receiver = read->args[0];
      lowerToStack(ctx, instructions, receiver);
      IridiumSEXP *field = read->args[1];
      lowerToStack(ctx, instructions, field);

      pushOP(ctx, instructions, OP_to_propkey2);
      pushOP(ctx, instructions, OP_dup2);
      pushOP(ctx, instructions, OP_get_array_el);
      if (isPrefix) {
        pushOP(ctx, instructions, isIncrement ? OP_inc : OP_dec);
        pushOP(ctx, instructions, OP_insert3);
      } else {
        pushOP(ctx, instructions, isIncrement ? OP_post_inc : OP_post_dec);
        pushOP(ctx, instructions, OP_perm4);
      }
      pushOP(ctx, instructions, OP_put_array_el);
    } else {
      // ---- obj.f++ / ++obj.f : lvalue depth = 1 ----
      ensureTag(read, "FieldRead");

      IridiumSEXP *field = read->args[1];
      ensureTag(field, "String");
      auto fieldNameString = getFlagString(field, "IridiumPrimitive");

      IridiumSEXP *receiver = read->args[0];
      lowerToStack(ctx, instructions, receiver);

      // obj -> obj, val   (get_field2 keeps the receiver for the write back;
      // never the OP_get_length fast path, since that consumes the receiver)
      JSAtom fieldAtom = JS_NewAtom(ctx, fieldNameString);
      pushOP32(ctx, instructions, OP_get_field2, fieldAtom);

      if (isPrefix) {
        pushOP(ctx, instructions, isIncrement ? OP_inc : OP_dec);
        pushOP(ctx, instructions, OP_insert2);
      } else {
        pushOP(ctx, instructions, isIncrement ? OP_post_inc : OP_post_dec);
        pushOP(ctx, instructions, OP_perm3);
      }

      JSAtom putAtom = JS_NewAtom(ctx, fieldNameString);
      pushOP32(ctx, instructions, OP_put_field, putAtom);
    }
  } else if (isTag(rval, "JSObjectProp")) {
    IridiumSEXP *val = rval->args[1];
    lowerToStack(ctx, instructions, val);

    IridiumSEXP *field = rval->args[0];
    ensureTag(field, "String");
    JSAtom fieldAtom =
        JS_NewAtom(ctx, getFlagString(field, "IridiumPrimitive"));
    return pushOP32(ctx, instructions, OP_define_field, fieldAtom);
  } else if (isTag(rval, "JSComputedObjectProp")) {
    IridiumSEXP *field = rval->args[0];
    lowerToStack(ctx, instructions, field);

    IridiumSEXP *val = rval->args[1];
    lowerToStack(ctx, instructions, val);

    pushOP(ctx, instructions, OP_define_array_el);
    return pushOP(ctx, instructions, OP_drop);
  } else if (isTag(rval, "JSObject")) {
    pushOP(ctx, instructions, OP_object);
  } else if (isTag(rval, "PoolBinding")) {
    uint32_t poolOffset = getFlagNumber(rval, "REFIDX");
    pushOP32(ctx, instructions, OP_fclosure, poolOffset);
    instructions.back().lambdaPoolReference = true;

    // if set name is true, set the name
    IridiumSEXP *lambdaSEXP = rval->args[0];
    ensureTag(lambdaSEXP, "Lambda");
    if (getFlagBoolean(lambdaSEXP, "SETNAME") &&
        getFlagBoolean(lambdaSEXP, "CNAME") == false) {
      auto nn = getFlagString(lambdaSEXP, "NAME");
      JSAtom lName = JS_NewAtom(ctx, nn);
      pushOP32(ctx, instructions, OP_set_name, lName);
    }
    return;
  } else if (isTag(rval, "JSClass")) {
    JSAtom classNameAtom = JS_NewAtom(ctx, getFlagString(rval, "NAME"));

    lowerToStack(ctx, instructions, rval->args[0]);

    // Instead of creating a closure, we push the constructor bytecode directly
    // onto the stack
    {
      IridiumSEXP *constructorClosure = rval->args[1];
      uint32_t poolOffset = getFlagNumber(constructorClosure, "REFIDX");
      pushOP32(ctx, instructions, OP_push_const, poolOffset);
      instructions.back().lambdaPoolReference = true;
    }

    uint8_t flags = hasFlag(rval, "DERIVED") ? 1 : 0;
    pushOP32Flags(ctx, instructions, OP_define_class, classNameAtom, flags);

    return;
  } else if (isTag(rval, "JSArray")) {
    if (rval->numArgs < 32) {
      for (int i = 0; i < rval->numArgs; i++) {
        lowerToStack(ctx, instructions, rval->args[i]);
      }
      return pushOP16(ctx, instructions, OP_array_from, rval->numArgs);
    } else {
      // 0 - 31
      for (int i = 0; i < 32; i++) {
        lowerToStack(ctx, instructions, rval->args[i]);
      }
      pushOP16(ctx, instructions, OP_array_from, 32);
      // > 31
      for (int i = 32; i < rval->numArgs; i++) {
        lowerToStack(ctx, instructions, rval->args[i]);
        JSAtom fieldAtom = JS_NewAtomUInt32(ctx, i);
        pushOP32(ctx, instructions, OP_define_field, fieldAtom);
      }
    }
  } else if (isTag(rval, "JSPrivate")) {
    char *data = getFlagString(rval, "IridiumPrimitive");
    JSAtom strAtom = JS_NewAtom(ctx, data);
    return pushOP32(ctx, instructions, OP_private_symbol, strAtom);
  } else if (isTag(rval, "JSPrivateFieldRead")) {
    lowerToStack(ctx, instructions, rval->args[0]);
    lowerToStack(ctx, instructions, rval->args[1]);
    // return pushOP(ctx, instructions, OP_get_private_field);
    return;
  } else if (isTag(rval, "JSSuperFieldRead")) {
    lowerToStack(ctx, instructions, rval->args[0]);
    lowerToStack(ctx, instructions, rval->args[1]);
    pushOP(ctx, instructions, OP_get_super);
    lowerToStack(ctx, instructions, rval->args[2]);
    return pushOP(ctx, instructions, OP_get_super_value);
  } else if (isTag(rval, "JSSuperFieldWrite")) {
    lowerToStack(ctx, instructions, rval->args[0]);
    lowerToStack(ctx, instructions, rval->args[1]);
    lowerToStack(ctx, instructions, rval->args[2]);
    lowerToStack(ctx, instructions, rval->args[3]);
    // this obj prop a -> a this obj prop a
    pushOP(ctx, instructions, OP_insert4);
    return pushOP(ctx, instructions, OP_put_super_value);
  } else if (isTag(rval, "Null")) {
    return pushOP(ctx, instructions, OP_null);
  } else if (isTag(rval, "JSBigInt")) {
    char *str = getFlagString(rval, "IridiumPrimitive");
    size_t len = strlen(str);

    // Strip trailing 'n' or 'N' (JS BigInt literal)
    if (len > 0 && (str[len - 1] == 'n' || str[len - 1] == 'N')) {
      str[len - 1] = '\0';
      len--;
    }

    // Detect radix
    int radix = 10;
    if (len > 2 && str[0] == '0') {
      if (str[1] == 'x' || str[1] == 'X') {
        radix = 16;
        memmove(str, str + 2, len - 1); // strip "0x"
      } else if (str[1] == 'o' || str[1] == 'O') {
        radix = 8;
        memmove(str, str + 2, len - 1); // strip "0o"
      } else if (str[1] == 'b' || str[1] == 'B') {
        radix = 2;
        memmove(str, str + 2, len - 1); // strip "0b"
      }
    }

    // Remove underscores (JS allows 1_000_000n)
    {
      char *read = str, *write = str;
      while (*read) {
        if (*read != '_')
          *write++ = *read;
        read++;
      }
      *write = '\0';
    }

    // Parse as BigInt
    JSBigInt *r = js_bigint_from_string(ctx, str, radix);
    if (!r) {
      JS_ThrowOutOfMemory(ctx);
      return;
    }

    JSValue val = JS_CompactBigInt(ctx, r);
    return pushOPConst(ctx, instructions, OP_push_const, val);
  } else if (isTag(rval, "Await")) {
    lowerToStack(ctx, instructions, rval->args[0]);
    return pushOP(ctx, instructions, OP_await);
  } else if (isTag(rval, "Nope")) {
    return;
  } else if (isTag(rval, "DCTRRet")) {
    // user_val
    lowerToStack(ctx, instructions, rval->args[0]);
    pushOP(ctx, instructions, OP_check_ctor_return);
    // user_val t/f
    pushOP8(ctx, instructions, OP_if_false8, 1);
    size_t ifElseTargetLoc = instructions.size() - 1;
    instructions.back().presolvedTarget = true;
    // ::true:
    pushOP(ctx, instructions, OP_drop);
    // this_val
    lowerToStack(ctx, instructions, rval->args[1]);
    // Determine false offset
    for (size_t i = ifElseTargetLoc + 1; i < instructions.size(); i++) {
      instructions[ifElseTargetLoc].data.one +=
          short_opcode_info(instructions[i].bc).size;
    }
  } else if (isTag(rval, "StackPop")) {
    return;
  } else if (isTag(rval, "JSForInNext")) {
    lowerToStack(ctx, instructions, rval->args[0]);
    pushOP(ctx, instructions, OP_for_in_next);

    // Keep 2, Nip 1
    return keepNDropM(ctx, instructions, 2, 1);
  } else if (isTag(rval, "JSCopyDataProperties")) {
    // exc_obj
    IridiumSEXP *exc_obj = rval->args[0];
    lowerToStack(ctx, instructions, exc_obj);

    // source
    IridiumSEXP *source = rval->args[1];
    lowerToStack(ctx, instructions, source);

    // target
    IridiumSEXP *target = rval->args[2];
    lowerToStack(ctx, instructions, target);

    // OP_copy_data_properties
    pushOP16(ctx, instructions, OP_copy_data_properties, 68);

    // Keep 1, Nip 2
    return keepNDropM(ctx, instructions, 1, 2);
  } else if (isTag(rval, "ThisINIT")) {
    // lowerToStack(ctx, instructions, rval->args[0]); We artifically create
    // this read dependency to make Liveness analysis cleaner
    return lowerToStack(ctx, instructions, rval->args[1]);
  } else if (isTag(rval, "JSAppend")) {
    lowerToStack(ctx, instructions, rval->args[0]); // tmp
    lowerToStack(ctx, instructions, rval->args[1]); // insertionIdx
    lowerToStack(ctx, instructions, rval->args[2]); // spreadVal
    return pushOP(ctx, instructions, OP_append);
  } else if (isTag(rval, "JSForOfNext")) {

    if (getFlagBoolean(rval, "AWAIT")) {
      pushOP(ctx, instructions, OP_dup3);
      pushOP(ctx, instructions, OP_drop);
      pushOP16(ctx, instructions, OP_call_method, 0);
      pushOP(ctx, instructions, OP_await);
      // [it, meth, off, <RESOLVED>] -> [it, meth, off, result, done]
      return pushOP(ctx, instructions, OP_iterator_get_value_done);
    } else {
      // [it, meth, off] -> [it, meth, off, result, done]
      return pushOP16(ctx, instructions, OP_for_of_next, 0);
    }
  } else if (isTag(rval, "JSCatchContext")) {
    // Do nothing
    return;
  } else {
    fprintf(stderr, "TODO: unhandled RVal: %s\n", rval->tag.data());
    exit(1);
  }
  return;
}

void peepholeOptimizeStackOPS(
    std::vector<BCInstruction> &instructions,
    std::unordered_map<uint32_t, size_t> &iriOffsetToStartInstMap) {
  for (size_t i = 0; i + 1 < instructions.size();) {
    auto &curr = instructions[i];
    auto &next = instructions[i + 1];

    // -------------------------------
    // Case 1: get_loc_0 + get_loc_1 → get_loc0_loc1
    // -------------------------------
    if (curr.bc == OP_get_loc0 && next.bc == OP_get_loc1) {

      // Safety Check
      bool safe = true;
      for (auto &e : iriOffsetToStartInstMap) {
        if (i + 1 == e.second) {
          safe = false;
          break;
        }
      }

      if (!safe) {
        ++i;
        continue;
      }

      curr.bc = OP_get_loc0_loc1;

      // curr.bc = OP_get_loc0_loc1;
      instructions.erase(instructions.begin() + i + 1);

      for (auto &e : iriOffsetToStartInstMap) {
        if (e.second > i) {
          iriOffsetToStartInstMap[e.first]--;
        }
      }

      for (auto &e : *gotoContextMap) {
        e.second.reduce(i);
      }

      continue;
    }

    // -------------------------------
    // Case 2: put_locX + get_locX → set_locX
    // -------------------------------
    if ((curr.bc == OP_put_loc0 && next.bc == OP_get_loc0) ||
        (curr.bc == OP_put_loc1 && next.bc == OP_get_loc1) ||
        (curr.bc == OP_put_loc2 && next.bc == OP_get_loc2) ||
        (curr.bc == OP_put_loc3 && next.bc == OP_get_loc3)) {

      // Safety Check
      bool safe = true;
      double iriContext = -1;
      for (auto &e : iriOffsetToStartInstMap) {
        if (i + 1 == e.second) {
          iriContext = e.first;
          safe = false;
          break;
        }
      }

      if (safe) {
        switch (curr.bc) {
        case OP_put_loc0:
          curr.bc = OP_set_loc0;
          break;
        case OP_put_loc1:
          curr.bc = OP_set_loc1;
          break;
        case OP_put_loc2:
          curr.bc = OP_set_loc2;
          break;
        case OP_put_loc3:
          curr.bc = OP_set_loc3;
          break;
        default:
          break;
        }
        instructions.erase(instructions.begin() + i + 1);

        for (auto &e : iriOffsetToStartInstMap) {
          if (e.second > i) {
            iriOffsetToStartInstMap[e.first]--;
          }
        }

        for (auto &e : *gotoContextMap) {
          e.second.reduce(i);
        }

        ++i;
        continue;
      } else {
        if (gotoContextMap->count(iriContext) == 0 ||
            (*gotoContextMap)[iriContext].kind == GOTOCONTEXT::MULTI) {
          ++i;
          continue;
        } else {
          if (GOTOINFO::getInstContext(curr) ==
              (*gotoContextMap)[iriContext].kind) {
            switch (curr.bc) {
            case OP_put_loc0:
              curr.bc = OP_set_loc0;
              break;
            case OP_put_loc1:
              curr.bc = OP_set_loc1;
              break;
            case OP_put_loc2:
              curr.bc = OP_set_loc2;
              break;
            case OP_put_loc3:
              curr.bc = OP_set_loc3;
              break;
            default:
              break;
            }

            instructions.erase(instructions.begin() + i + 1);

            for (auto &e : iriOffsetToStartInstMap) {
              if (e.second > i) {
                iriOffsetToStartInstMap[e.first]--;
              }
            }

            for (auto &e : *gotoContextMap) {
              e.second.reduce(i);
            }

            while ((*gotoContextMap)[iriContext].putTargets.size() > 0) {
              auto it = (*gotoContextMap)[iriContext].putTargets.begin();
              auto putTargetToRemove = *it;
              (*gotoContextMap)[iriContext].putTargets.erase(putTargetToRemove);

              instructions.erase(instructions.begin() + putTargetToRemove);

              for (auto &e : iriOffsetToStartInstMap) {
                if (e.second > putTargetToRemove) {
                  iriOffsetToStartInstMap[e.first]--;
                }
              }

              for (auto &e : *gotoContextMap) {
                e.second.reduce(putTargetToRemove);
              }
            }

            continue;
          } else {
            ++i;
            continue;
          }
        }
      }
    }

    if ((curr.bc == OP_put_arg0 && next.bc == OP_get_arg0) ||
        (curr.bc == OP_put_arg1 && next.bc == OP_get_arg1) ||
        (curr.bc == OP_put_arg2 && next.bc == OP_get_arg2) ||
        (curr.bc == OP_put_arg3 && next.bc == OP_get_arg3)) {

      // Safety Check
      bool safe = true;
      double iriContext = -1;
      for (auto &e : iriOffsetToStartInstMap) {
        if (i + 1 == e.second) {
          iriContext = e.first;
          safe = false;
          break;
        }
      }

      if (safe) {
        switch (curr.bc) {
        case OP_put_arg0:
          curr.bc = OP_set_arg0;
          break;
        case OP_put_arg1:
          curr.bc = OP_set_arg1;
          break;
        case OP_put_arg2:
          curr.bc = OP_set_arg2;
          break;
        case OP_put_arg3:
          curr.bc = OP_set_arg3;
          break;
        default:
          break;
        }
        instructions.erase(instructions.begin() + i + 1);

        for (auto &e : iriOffsetToStartInstMap) {
          if (e.second > i) {
            iriOffsetToStartInstMap[e.first]--;
          }
        }

        for (auto &e : *gotoContextMap) {
          e.second.reduce(i);
        }

        ++i;
        continue;
      } else {
        if (gotoContextMap->count(iriContext) == 0 ||
            (*gotoContextMap)[iriContext].kind == GOTOCONTEXT::MULTI) {
          ++i;
          continue;
        } else {
          if (GOTOINFO::getInstContext(curr) ==
              (*gotoContextMap)[iriContext].kind) {
            switch (curr.bc) {
            case OP_put_arg0:
              curr.bc = OP_set_arg0;
              break;
            case OP_put_arg1:
              curr.bc = OP_set_arg1;
              break;
            case OP_put_arg2:
              curr.bc = OP_set_arg2;
              break;
            case OP_put_arg3:
              curr.bc = OP_set_arg3;
              break;
            default:
              break;
            }
            instructions.erase(instructions.begin() + i + 1);

            for (auto &e : iriOffsetToStartInstMap) {
              if (e.second > i) {
                iriOffsetToStartInstMap[e.first]--;
              }
            }

            for (auto &e : *gotoContextMap) {
              e.second.reduce(i);
            }

            while ((*gotoContextMap)[iriContext].putTargets.size() > 0) {
              auto it = (*gotoContextMap)[iriContext].putTargets.begin();
              auto putTargetToRemove = *it;
              (*gotoContextMap)[iriContext].putTargets.erase(putTargetToRemove);

              instructions.erase(instructions.begin() + putTargetToRemove);

              for (auto &e : iriOffsetToStartInstMap) {
                if (e.second > putTargetToRemove) {
                  iriOffsetToStartInstMap[e.first]--;
                }
              }

              for (auto &e : *gotoContextMap) {
                e.second.reduce(putTargetToRemove);
              }
            }

            continue;
          } else {
            ++i;
            continue;
          }
        }
      }
    }

    if ((curr.bc == OP_put_var_ref0 && next.bc == OP_get_var_ref0) ||
        (curr.bc == OP_put_var_ref1 && next.bc == OP_get_var_ref1) ||
        (curr.bc == OP_put_var_ref2 && next.bc == OP_get_var_ref2) ||
        (curr.bc == OP_put_var_ref3 && next.bc == OP_get_var_ref3)) {

      // Safety Check
      bool safe = true;
      double iriContext = -1;
      for (auto &e : iriOffsetToStartInstMap) {
        if (i + 1 == e.second) {
          iriContext = e.first;
          safe = false;
          break;
        }
      }

      if (safe) {
        switch (curr.bc) {
        case OP_put_var_ref0:
          curr.bc = OP_set_var_ref0;
          break;
        case OP_put_var_ref1:
          curr.bc = OP_set_var_ref1;
          break;
        case OP_put_var_ref2:
          curr.bc = OP_set_var_ref2;
          break;
        case OP_put_var_ref3:
          curr.bc = OP_set_var_ref3;
          break;
        default:
          break;
        }
        instructions.erase(instructions.begin() + i + 1);

        for (auto &e : iriOffsetToStartInstMap) {
          if (e.second > i) {
            iriOffsetToStartInstMap[e.first]--;
          }
        }

        for (auto &e : *gotoContextMap) {
          e.second.reduce(i);
        }

        ++i;
        continue;
      } else {
        if (gotoContextMap->count(iriContext) == 0 ||
            (*gotoContextMap)[iriContext].kind == GOTOCONTEXT::MULTI) {
          ++i;
          continue;
        } else {
          if (GOTOINFO::getInstContext(curr) ==
              (*gotoContextMap)[iriContext].kind) {
            switch (curr.bc) {
            case OP_put_var_ref0:
              curr.bc = OP_set_var_ref0;
              break;
            case OP_put_var_ref1:
              curr.bc = OP_set_var_ref1;
              break;
            case OP_put_var_ref2:
              curr.bc = OP_set_var_ref2;
              break;
            case OP_put_var_ref3:
              curr.bc = OP_set_var_ref3;
              break;
            default:
              break;
            }
            instructions.erase(instructions.begin() + i + 1);

            for (auto &e : iriOffsetToStartInstMap) {
              if (e.second > i) {
                iriOffsetToStartInstMap[e.first]--;
              }
            }

            for (auto &e : *gotoContextMap) {
              e.second.reduce(i);
            }

            while ((*gotoContextMap)[iriContext].putTargets.size() > 0) {
              auto it = (*gotoContextMap)[iriContext].putTargets.begin();
              auto putTargetToRemove = *it;
              (*gotoContextMap)[iriContext].putTargets.erase(putTargetToRemove);

              instructions.erase(instructions.begin() + putTargetToRemove);

              for (auto &e : iriOffsetToStartInstMap) {
                if (e.second > putTargetToRemove) {
                  iriOffsetToStartInstMap[e.first]--;
                }
              }

              for (auto &e : *gotoContextMap) {
                e.second.reduce(putTargetToRemove);
              }
            }

            continue;
          } else {
            ++i;
            continue;
          }
        }
      }
    }
    ++i;
  }
}

void handleGWrite(JSContext *ctx, vector<BCInstruction> &instructions,
                  IridiumSEXP *currStmt, bool skipRVAL = false) {
  // "INIT", "SAFE", "DECLVAR", "DECLFUN"
  bool INIT = hasFlag(currStmt, "INIT");
  bool DECLVAR = hasFlag(currStmt, "DECLVAR");
  bool DECLFUN = hasFlag(currStmt, "DECLFUN");

  IridiumSEXP *loc = currStmt->args[0];
  ensureTag(loc, "GlobalBinding");
  IridiumSEXP *rval = currStmt->args[1];
  char *name = getFlagString(loc, "NAME");

  if (DECLVAR) {
    uint8_t check_flag = 0, define_flag = 0;

#define DEFINE_GLOBAL_LEX_VAR (1 << 7)
#define DEFINE_GLOBAL_FUNC_VAR (1 << 6)

    if (hasFlag(loc, "JSLET")) {
      check_flag |= DEFINE_GLOBAL_LEX_VAR;
      define_flag |= DEFINE_GLOBAL_LEX_VAR;
      define_flag |= JS_PROP_WRITABLE;
    } else if (hasFlag(loc, "JSCONST")) {
      check_flag |= DEFINE_GLOBAL_LEX_VAR;
      define_flag |= DEFINE_GLOBAL_LEX_VAR;
    } else if (hasFlag(loc, "JSVAR")) {
      // Redundant...
      check_flag = 0;
      check_flag = 0;
    } else {
      fprintf(stderr, "TODO: GWrite DECLVAR invalid flag config\n");
      exit(1);
    }

    pushOP32Flags(ctx, instructions, OP_check_define_var, JS_NewAtom(ctx, name),
                  check_flag);
    pushOP32Flags(ctx, instructions, OP_define_var, JS_NewAtom(ctx, name),
                  define_flag);
  } else if (DECLFUN) {
    pushOP32Flags(ctx, instructions, OP_check_define_var, JS_NewAtom(ctx, name),
                  64);
    if (skipRVAL == false)
      lowerToStack(ctx, instructions, rval);
    pushOP32Flags(ctx, instructions, OP_define_func, JS_NewAtom(ctx, name), 0);
  } else if (INIT) {
    if (skipRVAL == false)
      lowerToStack(ctx, instructions, rval);
    pushOP32(ctx, instructions, OP_put_var_init, JS_NewAtom(ctx, name));
  } else {
    if (skipRVAL == false)
      lowerToStack(ctx, instructions, rval);
    pushOP32(ctx, instructions, OP_put_var, JS_NewAtom(ctx, name));
  }
}

void handleLWrite(JSContext *ctx, vector<BCInstruction> &instructions,
                  IridiumSEXP *currStmt, bool skipRVAL = false) {
  // "INIT", "SAFE", "THISINIT"
  bool INIT = hasFlag(currStmt, "INIT");
  bool SAFE = hasFlag(currStmt, "SAFE");
  bool THISINIT = hasFlag(currStmt, "THISINIT");

  IridiumSEXP *loc = currStmt->args[0];
  ensureTag(loc, "EnvBinding");
  int refIDX = getFlagNumber(loc, "REFIDX");
  IridiumSEXP *rval = currStmt->args[1];

  if (skipRVAL == false)
    lowerToStack(ctx, instructions, rval);

  if (hasFlag(loc, "JSARG")) {
    assert(refIDX > -1);

    // === SPECIALIZATION ===
    switch (refIDX) {
    case 0:
      return pushOP(ctx, instructions, OP_put_arg0);
      break;
    case 1:
      return pushOP(ctx, instructions, OP_put_arg1);
      break;
    case 2:
      return pushOP(ctx, instructions, OP_put_arg2);
      break;
    case 3:
      return pushOP(ctx, instructions, OP_put_arg3);
      break;
    default:
      return pushOP16(ctx, instructions, OP_put_arg, refIDX);
    }
  }

  if (hasFlag(loc, "JSRESTARG")) {
    fprintf(stderr, "TODO: handle stores to JSRESTARG");
    exit(1);
  }

  if (THISINIT && !INIT) {
    return pushOP16(ctx, instructions, OP_put_loc_check_init, refIDX);
  }

  if (!INIT && hasFlag(loc, "JSCONST")) {
    pushOP(ctx, instructions, OP_drop);
    return pushOP32Flags(ctx, instructions, OP_throw_error,
                         JS_NewAtom(ctx, getFlagString(loc, "NAME")), 0);
  }

  if (SAFE || INIT) {
    // === SPECIALIZATION ===
    switch (refIDX) {
    case 0:
      return pushOP(ctx, instructions, OP_put_loc0);
    case 1:
      return pushOP(ctx, instructions, OP_put_loc1);
    case 2:
      return pushOP(ctx, instructions, OP_put_loc2);
    case 3:
      return pushOP(ctx, instructions, OP_put_loc3);
    default: {
      if (refIDX < 256) {
        return pushOP8(ctx, instructions, OP_put_loc8, refIDX);
      } else {
        return pushOP16(ctx, instructions, OP_put_loc, refIDX);
      }
    }
    }
  } else {
    return pushOP16(ctx, instructions, OP_put_loc_check, refIDX);
  }
}

void handleRMWrite(JSContext *ctx, vector<BCInstruction> &instructions,
                   IridiumSEXP *currStmt, bool skipRVAL = false) {
  // "INIT", "SAFE", "THISINIT"
  bool INIT = hasFlag(currStmt, "INIT");
  bool SAFE = hasFlag(currStmt, "SAFE");
  bool THISINIT = hasFlag(currStmt, "THISINIT");

  IridiumSEXP *loc = currStmt->args[0];
  ensureTag(loc, "RemoteEnvBinding");
  int refIDX = getFlagNumber(loc, "REFIDX");
  IridiumSEXP *rval = currStmt->args[1];
  IridiumSEXP *resolvedBinding;
  {
    resolvedBinding = loc->args[0];
    while (isTag(resolvedBinding, "RemoteEnvBinding")) {
      resolvedBinding = resolvedBinding->args[0];
    }

    ensureTag(resolvedBinding, "EnvBinding");
  }

  if (skipRVAL == false)
    lowerToStack(ctx, instructions, rval);

  if (THISINIT) {
    return pushOP16(ctx, instructions, OP_put_var_ref_check_init, refIDX);
  }

  if (!INIT && hasFlag(resolvedBinding, "JSCONST")) {
    pushOP(ctx, instructions, OP_drop);
    return pushOP32Flags(
        ctx, instructions, OP_throw_error,
        JS_NewAtom(ctx, getFlagString(resolvedBinding, "NAME")), 0);
  }

  if (SAFE || INIT) {
    switch (refIDX) {
    case 0:
      return pushOP(ctx, instructions, OP_put_var_ref0);
      break;
    case 1:
      return pushOP(ctx, instructions, OP_put_var_ref1);
      break;
    case 2:
      return pushOP(ctx, instructions, OP_put_var_ref2);
      break;
    case 3:
      return pushOP(ctx, instructions, OP_put_var_ref3);
      break;
    }

    return pushOP16(ctx, instructions, OP_put_var_ref, refIDX);
  } else {
    return pushOP16(ctx, instructions, OP_put_var_ref_check, refIDX);
  }
}

void handleIriStmt(JSContext *ctx, vector<BCInstruction> &instructions,
                   IridiumSEXP *currStmt) {

  // New AMP nodes
  if (isTag(currStmt, "EnvRead") || isTag(currStmt, "FieldRead") ||
      isTag(currStmt, "CallSite") || isTag(currStmt, "Apply") ||
      isTag(currStmt, "PVTEnvRead") || isTag(currStmt, "JSPrivateFieldRead") ||
      isTag(currStmt, "JSCatchContext") || isTag(currStmt, "JSBinop") ||
      isTag(currStmt, "JSUnop") || isTag(currStmt, "Unop") ||
      isTag(currStmt, "Binop") || isTag(currStmt, "JSComputedFieldRead") ||
      isTag(currStmt, "JSSuperFieldRead") || isTag(currStmt, "JSToObject") ||
      isTag(currStmt, "JSAppend") || isTag(currStmt, "JSCopyDataProperties") ||
      isTag(currStmt, "UNOPDelMemberExpr") || isTag(currStmt, "UNOPDelVar") ||
      isTag(currStmt, "Await") || isTag(currStmt, "IDOP") ||
      isTag(currStmt, "JSIDOP") || isTag(currStmt, "DCTRRet") ||
      isTag(currStmt, "ToNumeric")

      || isTag(currStmt, "JSTemplate")) {
    lowerToStack(ctx, instructions, currStmt);
    pushOP(ctx, instructions, OP_drop);
  } else if (isTag(currStmt, "Yield")) {
    fprintf(stderr, "Await not expected at stmt level...");
    exit(1);
  } else if (isTag(currStmt, "JSForOfStart")) {
    // Push obj onto the stack
    lowerToStack(ctx, instructions, currStmt->args[0]);

    // obj -> enum_obj iterator_method catch_offset
    if (getFlagBoolean(currStmt, "AWAIT")) {
      return pushOP(ctx, instructions, OP_for_await_of_start);
    } else {
      return pushOP(ctx, instructions, OP_for_of_start);
    }
  } else if (isTag(currStmt, "QJSModuleInit")) {
    pushOP(ctx, instructions, OP_push_this);
    pushOP32(ctx, instructions, OP_if_false8, 2);
    instructions.back().presolvedTarget = true;
    pushOP(ctx, instructions, OP_return_undef);
  } else if (isTag(currStmt, "GWrite")) {
    handleGWrite(ctx, instructions, currStmt, false);
  } else if (isTag(currStmt, "LWrite")) {
    handleLWrite(ctx, instructions, currStmt, false);
  } else if (isTag(currStmt, "RWrite") || isTag(currStmt, "MWrite")) {
    handleRMWrite(ctx, instructions, currStmt, false);
  } else if (isTag(currStmt, "JSSetPrototypeOf")) {
    IridiumSEXP *obj = currStmt->args[0]; // targetObj
    lowerToStack(ctx, instructions, obj);

    IridiumSEXP *proto = currStmt->args[1]; // protoValue
    lowerToStack(ctx, instructions, proto);

    pushOP(ctx, instructions, OP_set_proto);
    pushOP(ctx, instructions, OP_drop);
  } else if (isTag(currStmt, "JSDefineObjProp")) {
    IridiumSEXP *obj = currStmt->args[0];
    lowerToStack(ctx, instructions, obj);
    IridiumSEXP *field = currStmt->args[1];
    IridiumSEXP *val = currStmt->args[2];

    if (isTag(field, "String")) {
      lowerToStack(ctx, instructions, val);
      JSAtom fieldAtom =
          JS_NewAtom(ctx, getFlagString(field, "IridiumPrimitive"));
      pushOP32(ctx, instructions, OP_define_field, fieldAtom);
    } else {
      lowerToStack(ctx, instructions, field);
      lowerToStack(ctx, instructions, val);
      pushOP(ctx, instructions, OP_define_array_el);
      pushOP(ctx, instructions, OP_drop);
    }
    pushOP(ctx, instructions, OP_drop);
  } else if (isTag(currStmt, "JSDefineObjMethod")) {
    uint8_t op_flag;
    if (hasFlag(currStmt, "METHOD")) {
      op_flag = OP_DEFINE_METHOD_METHOD;
    } else if (hasFlag(currStmt, "GET")) {
      op_flag = OP_DEFINE_METHOD_GETTER;
    } else if (hasFlag(currStmt, "SET")) {
      op_flag = OP_DEFINE_METHOD_SETTER;
    } else {
      fprintf(stderr, "TODO: JSDefineObjMethod invalid flag\n");
      exit(1);
    }

    if (!hasFlag(currStmt, "NOENUM"))
      op_flag = op_flag | OP_DEFINE_METHOD_ENUMERABLE;

    IridiumSEXP *obj = currStmt->args[0];
    lowerToStack(ctx, instructions, obj);
    IridiumSEXP *field = currStmt->args[1];
    IridiumSEXP *val = currStmt->args[2];

    if (isTag(field, "String")) {
      lowerToStack(ctx, instructions, val);
      JSAtom fieldAtom =
          JS_NewAtom(ctx, getFlagString(field, "IridiumPrimitive"));
      pushOP32Flags(ctx, instructions, OP_define_method, fieldAtom, op_flag);
    } else {
      lowerToStack(ctx, instructions, field);
      lowerToStack(ctx, instructions, val);
      pushOPFlags(ctx, instructions, OP_define_method_computed, op_flag);
    }
    pushOP(ctx, instructions, OP_drop);
  } else if (isTag(currStmt, "JSADDBRAND")) {
    lowerToStack(ctx, instructions, currStmt->args[0]);
    lowerToStack(ctx, instructions, currStmt->args[1]);
    return pushOP(ctx, instructions, OP_add_brand);
  } else if (isTag(currStmt, "JSForOfIteratorClose")) {
    return pushOP(ctx, instructions, OP_iterator_close);
  } else if (isTag(currStmt, "JSSetHome")) {
    IridiumSEXP *homeObj = currStmt->args[0];
    lowerToStack(ctx, instructions, homeObj);

    IridiumSEXP *funcObj = currStmt->args[1];
    lowerToStack(ctx, instructions, funcObj);

    pushOP(ctx, instructions, OP_set_home_object);
    pushOP(ctx, instructions, OP_drop);
    pushOP(ctx, instructions, OP_drop);
  } else if (isTag(currStmt, "JSSetName")) {
    IridiumSEXP *obj = currStmt->args[0];
    lowerToStack(ctx, instructions, obj);
    IridiumSEXP *name = currStmt->args[1];
    if (isTag(name, "String")) {
      JSAtom fieldAtom =
          JS_NewAtom(ctx, getFlagString(name, "IridiumPrimitive"));
      pushOP32(ctx, instructions, OP_set_name, fieldAtom);
    } else {
      lowerToStack(ctx, instructions, name);
      pushOP(ctx, instructions, OP_set_name_computed);
    }
    pushOP(ctx, instructions, OP_drop);
  } else if (isTag(currStmt, "JSPrivateFieldWrite")) {
    handleJSPrivateFieldWrite(ctx, instructions, currStmt);
    pushOP(ctx, instructions, OP_drop);
  } else if (isTag(currStmt, "JSComputedFieldWrite")) {
    handleComputedFieldWrite(ctx, instructions, currStmt, false);
  } else if (isTag(currStmt, "FieldWrite")) {
    handleFieldWrite(ctx, instructions, currStmt, false);
  } else if (isTag(currStmt, "JSSuperFieldWrite") ||
             isTag(currStmt, "JSPrivateFieldWrite")) {
    lowerToStack(ctx, instructions, currStmt);
    return pushOP(ctx, instructions, OP_drop);
  } else if (isTag(currStmt, "Throw")) {
    lowerToStack(ctx, instructions, currStmt->args[0]);
    return pushOP(ctx, instructions, OP_throw);
  } else if (isTag(currStmt, "JSCheckConstructor")) {
    return pushOP(ctx, instructions, OP_check_ctor);
  }

  // else if (isTag(currStmt, "PushForOfCatchContext"))
  // {
  //   return lowerToStack(ctx, instructions, currStmt->args[0]);
  // }
  else if (isTag(currStmt, "IfJump")) {
    // Push check to stack
    lowerToStack(ctx, instructions, currStmt->args[0]);

    bool isNot = hasFlag(currStmt, "NOT");

    auto target = getFlagNumber(currStmt, "IDX");

    if ((*gotoContextMap).count(target) == 0)
      (*gotoContextMap)[target] = GOTOINFO();

    (*gotoContextMap)[target].kind = GOTOCONTEXT::MULTI;

    // Jmp to TRUE if stack value is true
    pushOP32(ctx, instructions, isNot ? OP_if_false : OP_if_true, target);

    return;
  } else if (isTag(currStmt, "IfElseJump")) {
    bool isNot = hasFlag(currStmt, "NOT");

    // Push check to stack
    lowerToStack(ctx, instructions, currStmt->args[0]);

    auto trueTarget = getFlagNumber(currStmt, "TRUE");
    auto falseTarget = getFlagNumber(currStmt, "FALSE");

    {
      if ((*gotoContextMap).count(trueTarget) == 0)
        (*gotoContextMap)[trueTarget] = GOTOINFO();

      (*gotoContextMap)[trueTarget].kind = GOTOCONTEXT::MULTI;
    }
    {
      if ((*gotoContextMap).count(falseTarget) == 0)
        (*gotoContextMap)[falseTarget] = GOTOINFO();

      (*gotoContextMap)[falseTarget].kind = GOTOCONTEXT::MULTI;
    }

    // Jmp to TRUE if stack value is true
    pushOP32(ctx, instructions, isNot ? OP_if_false : OP_if_true, trueTarget);

    // Jmp to FALSE if stack value is false
    pushOP32(ctx, instructions, OP_goto, falseTarget);

    return;
  } else if (isTag(currStmt, "JSModuleStart")) {
    pushOP(ctx, instructions, OP_push_this);
    pushOP8(ctx, instructions, OP_if_false8, 2);
    return pushOP(ctx, instructions, OP_return_undef);
  } else if (isTag(currStmt, "JSModuleEnd")) {
    pushOP(ctx, instructions, OP_undefined);
    return pushOP(ctx, instructions, OP_return_async);
  } else if (isTag(currStmt, "InvokeFinalizer")) {
    auto target = getFlagNumber(currStmt, "IDX");
    if ((*gotoContextMap).count(target) == 0)
      (*gotoContextMap)[target] = GOTOINFO();

    (*gotoContextMap)[target].kind = GOTOCONTEXT::MULTI;

    return pushOP32(ctx, instructions, OP_gosub, target);
  } else if (isTag(currStmt, "Goto")) {
    auto target = getFlagNumber(currStmt, "IDX");

    bool isPeepholeSafe = true;

    for (auto &e : *xyzz) {
      if (instructions.size() == e.second) {
        // This goto has incoming edges
        isPeepholeSafe = false;
      }
    }

    if (isPeepholeSafe) {
      if ((*gotoContextMap).count(target) > 0) {
        (*gotoContextMap)[target].merge(instructions);
      } else {
        (*gotoContextMap)[target] = GOTOINFO(instructions);
      }
    } else {
      (*gotoContextMap)[target].kind = GOTOCONTEXT::MULTI;
    }

    return pushOP32(ctx, instructions, OP_goto, target);
  } else if (isTag(currStmt, "PushCatchContext")) {
    auto target = getFlagNumber(currStmt, "IDX");
    if ((*gotoContextMap).count(target) == 0)
      (*gotoContextMap)[target] = GOTOINFO();

    (*gotoContextMap)[target].kind = GOTOCONTEXT::MULTI;

    return pushOP32(ctx, instructions, OP_catch, target);
  } else if (isTag(currStmt, "PopCatchContext") ||
             isTag(currStmt, "PopFinalizerReturnTarget")) {
    return pushOP(ctx, instructions, OP_drop);
  } else if (isTag(currStmt, "Return")) {
    lowerToStack(ctx, instructions, currStmt->args[0]);
    return pushOP(ctx, instructions, OP_return);
  } else if (isTag(currStmt, "ReturnAsync")) {
    lowerToStack(ctx, instructions, currStmt->args[0]);
    return pushOP(ctx, instructions, OP_return_async);
  } else if (isTag(currStmt, "NOP")) {
    return;
  } else if (isTag(currStmt, "JSInitialYield")) {
    return pushOP(ctx, instructions, OP_initial_yield);
  } else if (isTag(currStmt, "CompoundAssn")) {
    IridiumSEXP *rval = currStmt->args[0];
    lowerToStack(ctx, instructions, rval);
    for (int i = 1; i < currStmt->numArgs; i++) {
      IridiumSEXP *s = currStmt->args[i];
      if (isTag(s, "GWrite")) {
        handleGWrite(ctx, instructions, s, true);
      } else if (isTag(s, "LWrite")) {
        handleLWrite(ctx, instructions, s, true);
      } else if (isTag(s, "RWrite") || isTag(s, "MWrite")) {
        handleRMWrite(ctx, instructions, s, true);
      }
    }
  } else if (isTag(currStmt, "Ret")) {
    return pushOP(ctx, instructions, OP_ret);
  } else if (isTag(currStmt, "StackToHeap")) {
    for (int i = 0; i < currStmt->numArgs; i++) {
      IridiumSEXP *envBinding = currStmt->args[i];
      ensureTag(envBinding, "EnvBinding");
      int stackLocationIDX = getFlagNumber(envBinding, "REFIDX");
      pushOP16(ctx, instructions, OP_close_loc, stackLocationIDX);
    }
  } else {
    fprintf(stderr, "TODO: unhandled tag: %s\n", currStmt->tag.data());
    exit(1);
  }
  return;
}

void handleBB(JSContext *ctx, vector<BCInstruction> &instructions,
              IridiumSEXP *bb) {
  ensureTag(bb, "BB");

  for (int stmtIDX = 0; stmtIDX < bb->numArgs; stmtIDX++) {
    IridiumSEXP *currStmt = bb->args[stmtIDX];
    handleIriStmt(ctx, instructions, currStmt);
  }
  return;
}

// ============== Code Generation ============== //

// ============== Helper Functions ============== //

int getPoolSize(const vector<BCInstruction> &instructions) {
  int count = 0;
  for (size_t i = 0; i < instructions.size(); ++i) {
    auto &inst = instructions[i];
    if (inst.hasPoolData) {
      count++;
    }
  }
  return count;
}

int getBCSize(const vector<BCInstruction> &instructions) {
  int count = 0;
  for (size_t i = 0; i < instructions.size(); ++i) {
    auto &inst = instructions[i];
    count += short_opcode_info(inst.bc).size;
  }
  return count;
}

void populateCPool(JSContext *ctx, vector<BCInstruction> &instructions,
                   JSValue *cpool) {
  int offset = 0;
  for (size_t i = 0; i < instructions.size(); ++i) {
    auto &inst = instructions[i];
    if (inst.hasPoolData) {
      // fprintf(stdout, "Adding to cpool at offset %d\n", offset);
      *(cpool + offset) = JS_DupValue(ctx, inst.poolData);
      inst.data.four = offset;
      offset++;
    }
  }
}

void populateLambdaPoolReferences(JSContext *ctx,
                                  vector<BCInstruction> &instructions,
                                  int poolOffset) {
  for (size_t i = 0; i < instructions.size(); ++i) {
    auto &inst = instructions[i];
    if (inst.lambdaPoolReference) {
      if (inst.valueSize == 1) {
        int targetOffset = poolOffset + (inst.data.one);
        // fprintf(stdout, "Patching lambda offset %d to %d\n", inst.data.one,
        // targetOffset);
        inst.data.one = targetOffset;
      } else if (inst.valueSize == 2) {
        int targetOffset = poolOffset + (inst.data.two);
        // fprintf(stdout, "Patching lambda offset %d to %d\n", inst.data.two,
        // targetOffset);
        inst.data.two = targetOffset;
      } else if (inst.valueSize == 4) {
        int targetOffset = poolOffset + (inst.data.four);
        // fprintf(stdout, "Patching lambda offset %d to %d\n", inst.data.four,
        // targetOffset);
        inst.data.four = targetOffset;
      } else {
        fprintf(stderr,
                "Failed to patch lambda pool reference, no offset specified");
        exit(1);
      }
    }
  }
}

int findOffset(vector<BCInstruction> &instructions, int targetOffset,
               std::unordered_map<uint32_t, size_t> &iriOffsetToStartInstMap) {
  int offset = 0;

  if (iriOffsetToStartInstMap.count(targetOffset) == 0) {
    fprintf(stderr, "Failed to find BC offset for %d\n", targetOffset);
    exit(1);
  }

  for (size_t i = 0; i < iriOffsetToStartInstMap[targetOffset]; i++) {
    offset += short_opcode_info(instructions.at(i).bc).size;
  }

  return offset;
}

void patchGotos(vector<BCInstruction> &instructions,
                std::unordered_map<uint32_t, size_t> &iriOffsetToStartInstMap) {
  std::unordered_map<uint32_t, int> iriOffsetMap;
  int currOffset = 0;
  for (size_t i = 0; i < instructions.size(); ++i) {
    auto &inst = instructions[i];
    if (inst.bc == OP_goto || inst.bc == OP_catch || inst.bc == OP_gosub ||
        inst.bc == OP_if_true || inst.bc == OP_if_false) {
      if (!inst.presolvedTarget) {
        uint32_t iriOffset = inst.data.four;
        if (iriOffsetMap.count(iriOffset) == 0)
          iriOffsetMap[iriOffset] =
              findOffset(instructions, iriOffset, iriOffsetToStartInstMap);
        int actualOffset = iriOffsetMap[iriOffset];
        inst.data.four = actualOffset - currOffset - 1;
      }
      // fprintf(stdout, "Patching offset %d to %d : %d -- %d\n", iriOffset,
      // actualOffset, currOffset, inst.data.four);
    }
    currOffset += short_opcode_info(inst.bc).size;
  }
}

void freeBCLList(JSContext *ctx, vector<BCInstruction> &instructions) {
  for (auto &inst : instructions) {
    if (inst.hasPoolData) {
      JS_FreeValue(ctx, inst.poolData);
    }
  }
  instructions.clear();
  instructions.shrink_to_fit();
}

void populateBytecode(uint8_t *target,
                      const std::vector<BCInstruction> &instructions,
                      size_t startIndex, int &poolIDX) {
  size_t index = startIndex;

  while (index < instructions.size()) {
    const BCInstruction &currBC = instructions[index];

    if (currBC.hasPoolData) {
      // Note: This modifies poolIDX, but we can't modify the original data
      assert(currBC.valueSize == 4);
    }

    target[0] = currBC.bc;

    if (currBC.valueSize == 1) {
      uint8_t *t = (uint8_t *)(target + 1);
      *t = currBC.hasPoolData ? poolIDX++ : currBC.data.one;
    } else if (currBC.valueSize == 2) {
      uint16_t *t = (uint16_t *)(target + 1);
      *t = currBC.hasPoolData ? poolIDX++ : currBC.data.two;
    } else if (currBC.valueSize == 4) {
      uint32_t *t = (uint32_t *)(target + 1);
      *t = currBC.hasPoolData ? poolIDX++ : currBC.data.four;
    }

    if (currBC.hasFlags) {
      uint8_t *t = (uint8_t *)(target + short_opcode_info(currBC.bc).size - 1);
      *t = currBC.flags;
    }

    // Advance to next instruction
    target += short_opcode_info(currBC.bc).size;
    index++;
  }
}

// Alternative wrapper function to maintain similar interface
void populateBytecode(uint8_t *target,
                      const std::vector<BCInstruction> &instructions,
                      int poolIDX = 0) {
  int mutablePoolIDX = poolIDX;
  populateBytecode(target, instructions, 0, mutablePoolIDX);
}

typedef struct StackSizeState {
  int bc_len;
  int stack_len_max;
  uint16_t *stack_level_tab;
  int32_t *catch_pos_tab;
  int *pc_stack;
  int pc_stack_len;
  int pc_stack_size;
} StackSizeState;

int js_realloc_array(JSContext *ctx, void **parray, int elem_size, int *psize,
                     int req_size) {
  int new_size;
  size_t slack;
  void *new_array;
  /* XXX: potential arithmetic overflow */
  new_size = max_int(req_size, *psize * 3 / 2);
  new_array = js_realloc2(ctx, *parray, new_size * elem_size, &slack);
  if (!new_array)
    return -1;
  new_size += slack / elem_size;
  *psize = new_size;
  *parray = new_array;
  return 0;
}

int js_resize_array(JSContext *ctx, void **parray, int elem_size, int *psize,
                    int req_size) {
  if (unlikely(req_size > *psize))
    return js_realloc_array(ctx, parray, elem_size, psize, req_size);
  else
    return 0;
}

int ss_check(JSContext *ctx, StackSizeState *s, int pos, int op, int stack_len,
             int catch_pos) {
  if ((unsigned)pos >= s->bc_len) {
    JS_ThrowInternalError(ctx, "bytecode buffer overflow (op=%d, pc=%d)", op,
                          pos);
    return -1;
  }
  if (stack_len > s->stack_len_max) {
    s->stack_len_max = stack_len;
    if (s->stack_len_max > JS_STACK_SIZE_MAX) {
      JS_ThrowInternalError(ctx, "stack overflow (op=%d, pc=%d)", op, pos);
      return -1;
    }
  }
  if (s->stack_level_tab[pos] != 0xffff) {
    /* already explored: check that the stack size is consistent */
    if (s->stack_level_tab[pos] != stack_len) {
      JS_ThrowInternalError(ctx, "inconsistent stack size: %d %d (pc=%d)",
                            s->stack_level_tab[pos], stack_len, pos);
      return -1;
    } else if (s->catch_pos_tab[pos] != catch_pos) {
      JS_ThrowInternalError(ctx, "inconsistent catch position: %d %d (pc=%d)",
                            s->catch_pos_tab[pos], catch_pos, pos);
      return -1;
    } else {
      return 0;
    }
  }

  /* mark as explored and store the stack size */
  s->stack_level_tab[pos] = stack_len;
  s->catch_pos_tab[pos] = catch_pos;

  /* queue the new PC to explore */
  if (js_resize_array(ctx, (void **)&s->pc_stack, sizeof(s->pc_stack[0]),
                      &s->pc_stack_size, s->pc_stack_len + 1))
    return -1;
  s->pc_stack[s->pc_stack_len++] = pos;
  return 0;
}

int compute_stack_size(JSContext *ctx, uint8_t *bc_buf, int bcSize) {
  StackSizeState s_s, *s = &s_s;
  int i, diff, n_pop, pos_next, stack_len, pos, op, catch_pos, catch_level;
  const JSOpCode *oi;
  s->bc_len = bcSize;
  /* bc_len > 0 */
  s->stack_level_tab =
      (uint16_t *)js_malloc(ctx, sizeof(s->stack_level_tab[0]) * s->bc_len);
  if (!s->stack_level_tab)
    return -1;
  for (i = 0; i < s->bc_len; i++)
    s->stack_level_tab[i] = 0xffff;
  s->pc_stack = NULL;
  s->catch_pos_tab =
      (int32_t *)js_malloc(ctx, sizeof(s->catch_pos_tab[0]) * s->bc_len);
  if (!s->catch_pos_tab)
    goto fail;

  s->stack_len_max = 0;
  s->pc_stack_len = 0;
  s->pc_stack_size = 0;

  /* breadth-first graph exploration */
  if (ss_check(ctx, s, 0, OP_invalid, 0, -1))
    goto fail;

  while (s->pc_stack_len > 0) {
    pos = s->pc_stack[--s->pc_stack_len];
    stack_len = s->stack_level_tab[pos];
    catch_pos = s->catch_pos_tab[pos];
    op = bc_buf[pos];
    if (op == 0 || op >= OP_COUNT) {
      JS_ThrowInternalError(ctx, "invalid opcode (op=%d, pc=%d)", op, pos);
      goto fail;
    }
    oi = &short_opcode_info(op);
    // #ifdef ENABLE_DUMPS // JS_DUMP_BYTECODE_STACK
    //         if (check_dump_flag(ctx->rt, JS_DUMP_BYTECODE_STACK))
    // printf("%5d: %10s %5d %5d\n", pos, oi->name, stack_len, catch_pos);
    // #endif
    pos_next = pos + oi->size;
    if (pos_next > s->bc_len) {
      JS_ThrowInternalError(ctx, "bytecode buffer overflow (op=%d, pc=%d)", op,
                            pos);
      goto fail;
    }
    n_pop = oi->n_pop;
    /* call pops a variable number of arguments */
    if (oi->fmt == OP_FMT_npop || oi->fmt == OP_FMT_npop_u16) {
      n_pop += get_u16(bc_buf + pos + 1);
    } else if (oi->fmt == OP_FMT_npopx) {
      n_pop += op - OP_call0;
    }

    if (stack_len < n_pop) {
      JS_ThrowInternalError(ctx, "stack underflow (op=%d, pc=%d)", op, pos);
      goto fail;
    }
    stack_len += oi->n_push - n_pop;
    if (stack_len > s->stack_len_max) {
      s->stack_len_max = stack_len;
      if (s->stack_len_max > JS_STACK_SIZE_MAX) {
        JS_ThrowInternalError(ctx, "stack overflow (op=%d, pc=%d)", op, pos);
        goto fail;
      }
    }
    switch (op) {
    case OP_tail_call:
    case OP_tail_call_method:
    case OP_return:
    case OP_return_undef:
    case OP_return_async:
    case OP_throw:
    case OP_throw_error:
    case OP_ret:
      goto done_insn;
    case OP_goto:
      diff = get_u32(bc_buf + pos + 1);
      pos_next = pos + 1 + diff;
      break;
    case OP_goto16:
      diff = (int16_t)get_u16(bc_buf + pos + 1);
      pos_next = pos + 1 + diff;
      break;
    case OP_goto8:
      diff = (int8_t)bc_buf[pos + 1];
      pos_next = pos + 1 + diff;
      break;
    case OP_if_true8:
    case OP_if_false8:
      diff = (int8_t)bc_buf[pos + 1];
      if (ss_check(ctx, s, pos + 1 + diff, op, stack_len, catch_pos))
        goto fail;
      break;
    case OP_if_true:
    case OP_if_false:
      diff = get_u32(bc_buf + pos + 1);
      if (ss_check(ctx, s, pos + 1 + diff, op, stack_len, catch_pos))
        goto fail;
      break;
    case OP_gosub:
      diff = get_u32(bc_buf + pos + 1);
      if (ss_check(ctx, s, pos + 1 + diff, op, stack_len + 1, catch_pos))
        goto fail;
      break;
    case OP_with_get_var:
    case OP_with_delete_var:
      diff = get_u32(bc_buf + pos + 5);
      if (ss_check(ctx, s, pos + 5 + diff, op, stack_len + 1, catch_pos))
        goto fail;
      break;
    case OP_with_make_ref:
    case OP_with_get_ref:
    case OP_with_get_ref_undef:
      diff = get_u32(bc_buf + pos + 5);
      if (ss_check(ctx, s, pos + 5 + diff, op, stack_len + 2, catch_pos))
        goto fail;
      break;
    case OP_with_put_var:
      diff = get_u32(bc_buf + pos + 5);
      if (ss_check(ctx, s, pos + 5 + diff, op, stack_len - 1, catch_pos))
        goto fail;
      break;
    case OP_catch:
      diff = get_u32(bc_buf + pos + 1);
      if (ss_check(ctx, s, pos + 1 + diff, op, stack_len, catch_pos))
        goto fail;
      catch_pos = pos;
      break;
    case OP_for_of_start:
    case OP_for_await_of_start:
      catch_pos = pos;
      break;
      /* we assume the catch offset entry is only removed with
         some op codes */
    case OP_drop:
      catch_level = stack_len;
      goto check_catch;
    case OP_nip:
      catch_level = stack_len - 1;
      goto check_catch;
    case OP_nip1:
      catch_level = stack_len - 1;
      goto check_catch;
    case OP_iterator_close:
      catch_level = stack_len + 2;
    check_catch:
      /* Note: for for_of_start/for_await_of_start we consider
         the catch offset is on the first stack entry instead of
         the thirst */
      if (catch_pos >= 0) {
        int level;
        level = s->stack_level_tab[catch_pos];
        if (bc_buf[catch_pos] != OP_catch)
          level++; /* for_of_start, for_wait_of_start */
        /* catch_level = stack_level before op_catch is executed ? */
        if (catch_level == level) {
          catch_pos = s->catch_pos_tab[catch_pos];
        }
      }
      break;
    case OP_nip_catch:
      if (catch_pos < 0) {
        JS_ThrowInternalError(ctx, "nip_catch: no catch op (pc=%d)", pos);
        goto fail;
      }
      stack_len = s->stack_level_tab[catch_pos];
      if (bc_buf[catch_pos] != OP_catch)
        stack_len++; /* for_of_start, for_wait_of_start */
      stack_len++;   /* no stack overflow is possible by construction */
      catch_pos = s->catch_pos_tab[catch_pos];
      break;
    default:
      break;
    }
    if (ss_check(ctx, s, pos_next, op, stack_len, catch_pos))
      goto fail;

  done_insn:;
  }
  js_free(ctx, s->pc_stack);
  js_free(ctx, s->catch_pos_tab);
  js_free(ctx, s->stack_level_tab);
  return s->stack_len_max;
fail:
  js_free(ctx, s->pc_stack);
  js_free(ctx, s->catch_pos_tab);
  js_free(ctx, s->stack_level_tab);
  return 0;
}

// ============== Helper Functions ============== //

void dumpBCLList(JSContext *ctx, vector<BCInstruction> &instructions) {
  int i = 0;
  for (auto &inst : instructions) {
    fprintf(stdout, "BC[%d]: %s (size = %d bytes)", i,
            short_opcode_info(inst.bc).name, short_opcode_info(inst.bc).size);

    if (inst.bc == OP_push_const) {
      JSValue jsvalue = inst.poolData;
      fprintf(stdout, ", DATA_32: %d (\"%s\")\n", inst.data.four,
              JS_ToCString(ctx, jsvalue));
    } else if (inst.bc == OP_push_atom_value || inst.bc == OP_get_var ||
               inst.bc == OP_get_field) {
      fprintf(stdout, ", StringData(%d): \"%s\"\n", inst.data.four,
              JS_AtomToCString(ctx, inst.data.four));
    } else if (inst.bc == OP_fclosure) {
      fprintf(stdout, ", DATA_32: %d (<Closure>)\n", inst.data.four);
    } else {
      switch (short_opcode_info(inst.bc).size) {
      case 2:
        fprintf(stdout, ", DATA_8: %d\n", inst.data.one);
        break;
      case 3:
        fprintf(stdout, ", DATA_16: %d\n", inst.data.two);
        break;
      case 5:
        fprintf(stdout, ", DATA_32: %d\n", inst.data.four);
        break;
      default:
        fprintf(stdout, "\n");
      }
    }
    i += short_opcode_info(inst.bc).size;
  }
}

JSValue generateQjsFunction(JSContext *ctx, IridiumSEXP *bbContainer,
                            vector<BCInstruction> &instructions) {
  bool isStrict = hasFlag(bbContainer, "STRICT");

  IridiumSEXP *bindingsSEXP = bbContainer->args[0];
  ensureTag(bindingsSEXP, "Bindings");
  IridiumSEXP *localBindingsSEXP = bindingsSEXP->args[0];
  IridiumSEXP *remoteBindingsSEXP = bindingsSEXP->args[1];
  IridiumSEXP *lambdasSEXP = bindingsSEXP->args[2];

  // --- Setup ---
  int arg_count = 0;
  int var_count = 0;
  int closure_var_count = remoteBindingsSEXP->numArgs;
  int lambda_count = lambdasSEXP->numArgs;
  int bc_pool_count = getPoolSize(instructions);
  int cpool_count = bc_pool_count + lambda_count;
  int byte_code_len = getBCSize(instructions);

  // Initialize var/arg count
  for (int i = 0; i < localBindingsSEXP->numArgs; i++) {
    if (hasFlag(localBindingsSEXP->args[i], "JSARG") ||
        hasFlag(localBindingsSEXP->args[i], "JSRESTARG"))
      arg_count++;
    else
      var_count++;
  }

  // Compute layout offsets
  int function_size = sizeof(JSFunctionBytecode);
  int cpool_offset = function_size;
  function_size += cpool_count * sizeof(JSValue);
  int vardefs_offset = function_size;
  function_size += (arg_count + var_count) * sizeof(JSVarDef);
  int closure_var_offset = function_size;
  function_size += closure_var_count * sizeof(JSClosureVar);
  int byte_code_offset = function_size;
  function_size += byte_code_len;

  // Allocate function object
  JSFunctionBytecode *b = (JSFunctionBytecode *)js_mallocz(ctx, function_size);
  if (!b) {
    fprintf(stderr, "Failed to generate QJS function from Iridium code");
    exit(1);
  }

  b->header.ref_count = 1;
  b->header.gc_obj_type = JS_GC_OBJ_TYPE_FUNCTION_BYTECODE;

  // Allocate and initialize memory sections
  if (cpool_count > 0) {
    b->cpool = (JSValue *)((uint8_t *)b + cpool_offset);
    for (int i = 0; i < cpool_count; i++)
      b->cpool[i] = JS_UNDEFINED;
  }
  b->cpool_count = cpool_count;

  if (arg_count + var_count > 0) {
    b->vardefs = (JSVarDef *)((uint8_t *)b + vardefs_offset);
    b->arg_count = arg_count;
    b->var_count = var_count;
    // b->defined_arg_count = 0;
  }

  if (closure_var_count > 0) {
    b->closure_var = (JSClosureVar *)((uint8_t *)b + closure_var_offset);
    b->closure_var_count = closure_var_count;
  }

  b->byte_code_buf = (uint8_t *)b + byte_code_offset;
  b->byte_code_len = byte_code_len;

  // Set defined args count
  int ecmaArgsCount = getFlagNumber(bbContainer, "ECMAArgs");
  b->defined_arg_count = ecmaArgsCount;

  // Metadata
  b->func_name = JS_NewAtom(ctx, getFlagString(bbContainer, "NAME"));
  b->filename = JS_ATOM_NULL;
  b->line_num = 1;
  b->col_num = 1;

  // Optional
  b->stack_size = 0;
  b->source = js_strdup(ctx, "<TestFunc>");
  b->source_len = strlen(b->source);
  b->pc2line_buf = NULL;
  b->pc2line_len = 0;

  // Function flags
  b->is_strict_mode = isStrict;
  b->has_prototype = 0;
  b->has_simple_parameter_list = 0;
  b->is_derived_class_constructor = 0;
  b->need_home_object = 0;
  b->func_kind = JS_FUNC_NORMAL;
  b->new_target_allowed = 0;
  b->super_call_allowed = 0;
  b->super_allowed = 0;
  b->arguments_allowed = 0;
  b->backtrace_barrier = 0;

  // Realm
  b->realm = JS_DupContext(ctx);

  // Populate Cpool
  populateCPool(ctx, instructions, b->cpool);
  populateLambdaPoolReferences(ctx, instructions, bc_pool_count);
  // Populate Bytecode and compute stack size
  populateBytecode(b->byte_code_buf, instructions, 0);

  b->stack_size = compute_stack_size(ctx, b->byte_code_buf, b->byte_code_len);

  // Initialize Arg + Var Defs
  for (int i = 0; i < arg_count + var_count; i++) {
    IridiumSEXP *envBinding = localBindingsSEXP->args[i];
    ensureTag(envBinding, "EnvBinding");

    // int refIDX = getFlagNumber(envBinding, "REFIDX");
    // assert(refIDX == i && "Local VarDef idx not found");
    int scope_level = getFlagNumber(envBinding, "SCOPE");
    int scope_next =
        hasFlag(envBinding, "NEXT") ? getFlagNumber(envBinding, "NEXT") : -1;
    char *name = getFlagString(envBinding, "NAME");

    b->vardefs[i].var_name = JS_NewAtom(ctx, name);
    b->vardefs[i].scope_level = scope_level;
    b->vardefs[i].scope_next = scope_next;
    b->vardefs[i].var_kind = JS_VAR_NORMAL;

    b->vardefs[i].is_const = 0;
    b->vardefs[i].is_lexical = 0;
    b->vardefs[i].is_captured = 0;
    b->vardefs[i].is_static_private = 0;

    if (hasFlag(envBinding, "JSVAR") || hasFlag(envBinding, "JSARG") ||
        hasFlag(envBinding, "JSRESTARG")) {
      // fprintf(stderr, "JSARG not expected...");
      // exit(1);
      // NONE
    } else if (hasFlag(envBinding, "JSLET")) {
      b->vardefs[i].is_lexical = true;
    } else if (hasFlag(envBinding, "JSCONST")) {
      b->vardefs[i].is_lexical = true;
      b->vardefs[i].is_const = true;
    } else {
      fprintf(stderr, "Valid flag not found...");
      exit(1);
    }
  }

  // Initialize Closuer Var Defs
  for (int i = 0; i < closure_var_count; i++) {
    IridiumSEXP *remoteBinding = remoteBindingsSEXP->args[i];
    ensureTag(remoteBinding, "RemoteEnvBinding");

    // MODULE | MODULEI | MODULENSI
    bool MODULE = hasFlag(remoteBinding, "MODULE");
    bool MODULEI = hasFlag(remoteBinding, "MODULEI");
    bool MODULENSI = hasFlag(remoteBinding, "MODULENSI");
    int depth = 0;

    IridiumSEXP *resolvedBinding = remoteBinding->args[0];
    int REFIDX = getFlagNumber(resolvedBinding, "REFIDX");

    while (isTag(resolvedBinding, "RemoteEnvBinding")) {
      depth++;
      resolvedBinding = resolvedBinding->args[0];
    }
    ensureTag(resolvedBinding, "EnvBinding");

    int VARIDX = getFlagNumber(resolvedBinding, "REFIDX");
    char *name = getFlagString(resolvedBinding, "NAME");
    bool JSARG = hasFlag(resolvedBinding, "JSARG");
    bool JSRESTARG = hasFlag(resolvedBinding, "JSRESTARG");
    bool JSLET = hasFlag(resolvedBinding, "JSLET");
    bool JSCONST = hasFlag(resolvedBinding, "JSCONST");
    bool JSVAR = hasFlag(resolvedBinding, "JSVAR");

    if (MODULE) {
      b->closure_var[i].is_local = true;
      b->closure_var[i].is_arg = false;
      b->closure_var[i].is_const = JSCONST;
      b->closure_var[i].is_lexical = JSLET || JSVAR;
      b->closure_var[i].var_kind = JS_VAR_NORMAL;
      b->closure_var[i].var_idx = VARIDX;
      b->closure_var[i].var_name = JS_NewAtom(ctx, name);
    } else if (MODULEI) {
      b->closure_var[i].is_local = false;
      b->closure_var[i].is_arg = false;
      b->closure_var[i].is_const = JSCONST;
      b->closure_var[i].is_lexical = JSLET || JSVAR;
      b->closure_var[i].var_kind = JS_VAR_NORMAL;
      b->closure_var[i].var_idx = VARIDX;
      b->closure_var[i].var_name = JS_NewAtom(ctx, name);
    } else if (MODULENSI) {
      b->closure_var[i].is_local = true;
      b->closure_var[i].is_arg = false;
      b->closure_var[i].is_const = JSCONST;
      b->closure_var[i].is_lexical = JSLET || JSVAR;
      b->closure_var[i].var_kind = JS_VAR_NORMAL;
      b->closure_var[i].var_idx = VARIDX;
      b->closure_var[i].var_name = JS_NewAtom(ctx, name);
    } else {
      b->closure_var[i].is_local = depth == 0;
      b->closure_var[i].is_arg = JSARG || JSRESTARG;
      b->closure_var[i].is_const = JSCONST;
      b->closure_var[i].is_lexical = JSLET || JSVAR;
      b->closure_var[i].var_kind = JS_VAR_NORMAL;
      b->closure_var[i].var_idx = REFIDX;
      b->closure_var[i].var_name = JS_NewAtom(ctx, name);
    }
  }

  // printf("ADDR: %p\n", b);

  // Set fun kind
  if (hasFlag(bbContainer, "GENERATOR") && hasFlag(bbContainer, "ASYNC")) {
    b->func_kind = JS_FUNC_ASYNC_GENERATOR;
  } else if (hasFlag(bbContainer, "GENERATOR")) {
    b->func_kind = JS_FUNC_GENERATOR;
  } else if (hasFlag(bbContainer, "ASYNC")) {
    b->func_kind = JS_FUNC_ASYNC;
  }

  // Set special flags
  if (hasFlag(bbContainer, "ARGUMENTS")) {
    b->arguments_allowed = 1;
  }

  if (hasFlag(bbContainer, "PROTO")) {
    b->has_prototype = 1;
  }

  if (hasFlag(bbContainer, "NEW")) {
    b->new_target_allowed = 1;
  }

  if (hasFlag(bbContainer, "SCALL")) {
    b->super_call_allowed = 1;
  }

  if (hasFlag(bbContainer, "SOBJ")) {
    b->super_allowed = 1;
  }

  if (hasFlag(bbContainer, "HOME")) {
    b->need_home_object = 1;
  }

  if (hasFlag(bbContainer, "DERIVED")) {
    b->is_derived_class_constructor = 1;
  }

  // Register with GC
  add_gc_object(ctx->rt, &b->header, JS_GC_OBJ_TYPE_FUNCTION_BYTECODE);

  // Wrap into JSValue
  JSValue func_val = JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b);
  return func_val;
}

struct SnipSnap {
  std::vector<BCInstruction *> insts;
  std::unordered_map<BCInstruction *, int> bcOffsets;

  bool snipped;

  SnipSnap(std::vector<BCInstruction> &instructions) : snipped(true) {
    int offset = 0;
    for (size_t i = 0; i < instructions.size(); i++) {
      auto &inst = instructions.at(i);
      if (inst.bc == OP_goto || inst.bc == OP_catch || inst.bc == OP_gosub ||
          inst.bc == OP_if_true || inst.bc == OP_if_false) {
        insts.push_back(&inst);
        bcOffsets[&inst] = offset;
      }
      offset += short_opcode_info(instructions.at(i).bc).size;
    }
  }

  void printBCInst(BCInstruction *inst) {
    std::cout << "  [" << bcOffsets[inst] << "] ";
    if (inst->bc == OP_goto)
      std::cout << "OP_goto (" << (int)inst->data.four << ")";
    if (inst->bc == OP_goto8)
      std::cout << "OP_goto8 (" << (int)inst->data.one << ")";
    if (inst->bc == OP_goto16)
      std::cout << "OP_goto16 (" << (int)inst->data.two << ")";
    if (inst->bc == OP_catch)
      std::cout << "OP_catch (" << (int)inst->data.four << ")";
    if (inst->bc == OP_gosub)
      std::cout << "OP_gosub (" << (int)inst->data.four << ")";
    if (inst->bc == OP_if_true)
      std::cout << "OP_if_true (" << (int)inst->data.four << ")";
    if (inst->bc == OP_if_false)
      std::cout << "OP_if_false (" << (int)inst->data.four << ")";
    if (inst->bc == OP_if_true8)
      std::cout << "OP_if_true8 (" << (int)inst->data.one << ")";
    if (inst->bc == OP_if_false8)
      std::cout << "OP_if_false8 (" << (int)inst->data.one << ")";
    std::cout << std::endl;
  }

  void snip(int idx, int snipSize) {
    snipped = true;
    BCInstruction *snippedInst = insts[idx];
    int snipBCOffset = bcOffsets[snippedInst];

    // Decrement bcOffset of Insts that come after the SNIP
    for (int i = idx + 1; i < insts.size(); i++) {
      BCInstruction *iii = insts[i];
      bcOffsets[iii] = bcOffsets[iii] - snipSize;
    }

    // Algo,... Probably there are better ways to express this.. im sure
    for (int i = 0; i < insts.size(); i++) {
      if (i == idx)
        continue;

      BCInstruction *iii = insts[i];

      int32_t target = bcOffsets[iii];

      if (iii->valueSize == 1) {
        target += iii->data.one;
      } else if (iii->valueSize == 2) {
        target += iii->data.two;
      } else if (iii->valueSize == 4) {
        target += iii->data.four;
      } else
        assert(false && "Expected data at jump insts");

      if (i > idx) {
        if (target < snipBCOffset) {
          if (iii->valueSize == 1) {
            if (iii->data.one > 0)
              iii->data.one -= snipSize;
            else
              iii->data.one += snipSize;
          } else if (iii->valueSize == 2) {
            if (iii->data.two > 0)
              iii->data.two -= snipSize;
            else
              iii->data.two += snipSize;
          } else if (iii->valueSize == 4) {
            if (iii->data.four > 0)
              iii->data.four -= snipSize;
            else
              iii->data.four += snipSize;
          }
        }
      } else {
        if (target > snipBCOffset) {
          if (iii->valueSize == 1) {
            if (iii->data.one > 0)
              iii->data.one -= snipSize;
            else
              iii->data.one += snipSize;
          } else if (iii->valueSize == 2) {
            if (iii->data.two > 0)
              iii->data.two -= snipSize;
            else
              iii->data.two += snipSize;
          } else if (iii->valueSize == 4) {
            if (iii->data.four > 0)
              iii->data.four -= snipSize;
            else
              iii->data.four += snipSize;
          }
        }
      }
    }
  }

  void execute() {
    while (snipped == true) {
      snipped = false;
      for (int i = 0; i < insts.size(); i++) { // snap
        auto &inst = insts[i];

        if (inst->bc == OP_goto) {
          auto distance = inst->data.four;

          if (distance >= INT8_MIN && distance <= INT8_MAX) {
            // Snip 3 Bytes
            int SNIP = 3;

            inst->bc = OP_goto8;
            inst->valueSize = 1;

            if (distance > 0)
              inst->data.one = distance - SNIP;
            else
              inst->data.one = distance;

            snip(i, SNIP);
          } else if (distance >= INT16_MIN && distance <= INT16_MAX) {
            // Snip 2 Bytes
            int SNIP = 2;
            inst->bc = OP_goto16;
            inst->valueSize = 2;

            if (distance > 0)
              inst->data.two = distance - SNIP;
            else
              inst->data.two = distance;

            snip(i, SNIP);
          }
        } else if (inst->bc == OP_if_true || inst->bc == OP_if_false) {
          auto distance = inst->data.four;
          if (distance >= INT8_MIN && distance <= INT8_MAX) {
            // Snip 3 Bytes
            int SNIP = 3;
            inst->bc = inst->bc == OP_if_true ? OP_if_true8 : OP_if_false8;
            inst->valueSize = 1;
            if (distance > 0)
              inst->data.one = distance - SNIP;
            else
              inst->data.one = distance;
            snip(i, SNIP);
          }
        } else if (inst->bc == OP_goto16) {
          auto distance = inst->data.two;
          if (distance >= INT8_MIN && distance <= INT8_MAX) {
            // Snip 1 Byte
            int SNIP = 1;
            inst->bc = OP_goto8;
            inst->valueSize = 1;
            if (distance > 0)
              inst->data.one = distance - SNIP;
            else
              inst->data.one = distance;
            snip(i, SNIP);
          }
        }
      }
    }
  }
};

// Modified generateBytecode function with optimizations
JSValue generateBytecode(JSContext *ctx, IridiumSEXP *node) {
  IridiumSEXP *file = node;
  ensureTag(file, "File");
  // ensureFlag(file, "JSModule");

  // Entry 1: Module Requests
  // Entry 2: Static Imports
  // Entry 3: Static Exports
  // Entry 4: Static Star Exports
  uint8_t moduleMetaEntries = 4;
  uint16_t numModules = node->numArgs - moduleMetaEntries;

  JSValue *moduleList = (JSValue *)malloc(node->numArgs * sizeof(JSValue));
  int topLevelModuleIdx = -1;

  for (int i = 0; i < numModules; ++i) {
    IridiumSEXP *bbContainer = file->args[moduleMetaEntries + i];
    ensureTag(bbContainer, "BBContainer");

    bool isTopLevelModule = hasFlag(bbContainer, "TopLevel");
    if (isTopLevelModule) {
      topLevelModuleIdx = i;
    }

    vector<BCInstruction> instructions;

    // Generate initial bytecode
    IridiumSEXP *bbList = bbContainer->args[1];

    std::unordered_map<uint32_t, size_t> iriOffsetToStartInstMap;
    xyzz = &iriOffsetToStartInstMap;

    std::unordered_map<double, GOTOINFO> cMap;
    gotoContextMap = &cMap;

    for (int idx = 0; idx < bbList->numArgs; idx++) {
      IridiumSEXP *bb = bbList->args[idx];
      ensureTag(bb, "BB");

      iriOffsetToStartInstMap[getFlagNumber(bb, "IDX")] = instructions.size();

      for (int stmtIDX = 0; stmtIDX < bb->numArgs; stmtIDX++) {
        IridiumSEXP *currStmt = bb->args[stmtIDX];
        handleIriStmt(ctx, instructions, currStmt);
      }
    }

    for (auto it = cMap.begin(); it != cMap.end();) {
      if (it->second.kind == GOTOCONTEXT::MULTI ||
          it->second.putTargets.size() == 0) {
        it = cMap.erase(it);
      } else {
        ++it;
      }
    }

    // std::cout << "GOTO contexts (original)" << std::endl;
    // for (auto &e : cMap)
    // {
    //   std::cout << e.first << " " << e.second.kind << "( ";
    //   for (auto & pt : e.second.putTargets)
    //   {
    //     std::cout << short_opcode_info(instructions[pt].bc).name << " ";
    //   }
    //   std::cout << ")" << std::endl;
    // }
    // std::cout << "====== ====== ====== ====== ====== ====== " << std::endl;

    // // Apply optimizations in sequence
    // std::cout << "Before Peephole";
    // dumpBCLList(ctx, instructions);

    peepholeOptimizeStackOPS(instructions, iriOffsetToStartInstMap);

    gotoContextMap = NULL;

    // std::cout << "After Peephole";
    // dumpBCLList(ctx, instructions);

    // Snip Snap
    patchGotos(instructions, iriOffsetToStartInstMap);

    SnipSnap snipSnap(instructions);
    snipSnap.execute();

    // Generate the function with optimized instructions
    JSValue res = generateQjsFunction(ctx, bbContainer, instructions);

    // Free BCLList
    freeBCLList(ctx, instructions);

    // js_dump_function_bytecode(ctx, (JSFunctionBytecode *)res.u.ptr);

    moduleList[i] = res;
  }

  assert(topLevelModuleIdx >= 0);

  // Fill CPool with closures (same as before)
  for (int i = 0; i < numModules; i++) {
    IridiumSEXP *bbContainer = file->args[moduleMetaEntries + i];
    ensureTag(bbContainer, "BBContainer");

    IridiumSEXP *bindingsInfo = bbContainer->args[0];
    ensureTag(bindingsInfo, "Bindings");

    JSValue targetClosure = moduleList[i];
    JSFunctionBytecode *targetClosurePtr =
        (JSFunctionBytecode *)targetClosure.u.ptr;

    IridiumSEXP *lambdasList = bindingsInfo->args[2];

    int poolStartIdx = targetClosurePtr->cpool_count - lambdasList->numArgs;

    for (int j = 0; j < lambdasList->numArgs; j++) {
      IridiumSEXP *poolBinding = lambdasList->args[j];
      ensureTag(poolBinding, "PoolBinding");

      IridiumSEXP *lambdaSEXP = poolBinding->args[0];
      ensureTag(lambdaSEXP, "Lambda");

      // StartBBIDX of Closure that is needed
      int targetStartBBIDX = getFlagNumber(lambdaSEXP, "StartBBIDX");

      // Find the location of the target closure
      JSValue res;
      bool found = false;
      for (int k = 0; k < numModules; k++) {
        IridiumSEXP *bbContainer = file->args[moduleMetaEntries + k];
        ensureTag(bbContainer, "BBContainer");
        int closureStartBBIDX = getFlagNumber(bbContainer, "StartBBIDX");
        if (closureStartBBIDX == targetStartBBIDX) {
          res = moduleList[k];
          found = true;
        }
      }
      assert(found);

      // Patch cpool to point to this closures
      targetClosurePtr->cpool[poolStartIdx + j] = res;
    }
  }

  if (check_dump_flag(ctx, JS_DUMP_BYTECODE_FINAL)) {
    fprintf(stdout, "[Iridium] Dumping all non-topLevel module code\n");
  }

  for (int i = 0; i < numModules; i++) {
    if (i == topLevelModuleIdx)
      continue;
#ifdef LINKING_DUMP_FUNCTION
    JSValue funBC = moduleList[i];
    JSFunctionBytecode *b = (JSFunctionBytecode *)funBC.u.ptr;
    if (check_dump_flag(ctx, JS_DUMP_BYTECODE_FINAL)) {
      js_dump_function_bytecode(ctx, b);
    }
#endif
  }

  return moduleList[topLevelModuleIdx];
}

typedef struct IridiumLoadResult {
  bool isModule;
  void *ptr;
} IridiumLoadResult;

IridiumLoadResult compile_iri_module(JSContext *ctx, json &j) {
  // 1. Combine existence and type-checking effortlessly
  if (!j.contains("iridium") || !j["iridium"].is_array()) {
    fprintf(stderr, "Expected the 'iridium' key to be an array...\n");
    exit(1);
  }

  IridiumSEXP *iridiumCode;
  {
    // ScopedTimer t("parseIridiumSEXP");
    // Note: You will need to update the signature of parseIridiumSEXP
    // to take `const json&` instead of `cJSON*`
    iridiumCode = parseIridiumSEXP(j["iridium"]);
  }

  // Generate BC
  JSValue moduleFunVal;
  {
    // ScopedTimer t("generateBytecode");
    moduleFunVal = generateBytecode(ctx, iridiumCode);
  }

  JSFunctionBytecode *b = (JSFunctionBytecode *)moduleFunVal.u.ptr;
  bool isModule = hasFlag(iridiumCode, "JSModule");

  if (!isModule) {
#ifdef LINKING_DUMP_FUNCTION
    if (check_dump_flag(ctx, JS_DUMP_BYTECODE_FINAL)) {
      js_dump_function_bytecode(ctx, b);
    }
#endif
    return ((IridiumLoadResult){false, b});
  }

  // Module mode code
  b->func_kind = JS_FUNC_ASYNC;

  // 2. Safely extract the string (also checking it exists and is a string)
  if (!j.contains("absoluteFilePath") || !j["absoluteFilePath"].is_string()) {
    fprintf(stderr, "Expected 'absoluteFilePath' to be a string...\n");
    exit(1);
  }

  // Store it in a std::string so the memory is safely managed
  std::string absoluteFilePath = j["absoluteFilePath"].get<std::string>();

  // 3. Execute the file using the .c_str() method to satisfy QuickJS's C-API
  JSModuleDef *m =
      js_new_module_def(ctx, JS_NewAtom(ctx, absoluteFilePath.c_str()));

  // Bytecode container gets the filename
  b->filename = JS_NewAtom(ctx, absoluteFilePath.c_str());

  // Initialize Module
  IridiumSEXP *moduleRequests = iridiumCode->args[0];
  IridiumSEXP *staticImports = iridiumCode->args[1];
  IridiumSEXP *staticExports = iridiumCode->args[2];
  IridiumSEXP *starExports = iridiumCode->args[3];

  if (moduleRequests->numArgs > 0) {
    // 1. Create External Module Requests
    m->req_module_entries_count = moduleRequests->numArgs;
    m->req_module_entries_size = m->req_module_entries_count;
    m->req_module_entries = (JSReqModuleEntry *)js_mallocz(
        ctx, sizeof(m->req_module_entries[0]) * m->req_module_entries_size);

    for (int r = 0; r < moduleRequests->numArgs; ++r) {
      IridiumSEXP *moduleRequest = moduleRequests->args[r];
      char *SOURCE = getFlagString(moduleRequest, "SOURCE");
      m->req_module_entries[r].module_name = JS_NewAtom(ctx, SOURCE);
      m->req_module_entries[r].module = NULL;
    }
  }

  if (staticImports->numArgs > 0) {
    // 2. Link Static Import Targets
    m->import_entries_count = staticImports->numArgs;
    m->import_entries_size = m->import_entries_count;
    m->import_entries = (JSImportEntry *)js_mallocz(
        ctx, sizeof(m->import_entries[0]) * m->import_entries_size);

    for (int im = 0; im < staticImports->numArgs; ++im) {
      IridiumSEXP *staticImport = staticImports->args[im];

      // Req Module IDX
      int reqModuleIDX = getFlagNumber(staticImport, "MODULEREQIDX");

      // Target IDX
      IridiumSEXP *resolvedBinding = staticImport->args[0];
      ensureTag(resolvedBinding, "RemoteEnvBinding");
      int bindingTargetIDX = getFlagNumber(resolvedBinding, "REFIDX");

      // Field
      char *fieldName = getFlagString(staticImport, "FIELD");

      m->import_entries[im].var_idx = bindingTargetIDX;
      m->import_entries[im].import_name = JS_NewAtom(ctx, fieldName);
      m->import_entries[im].req_module_idx = reqModuleIDX;
    }
  }

  if (staticExports->numArgs > 0) {
    // 3. Link Exports
    m->export_entries_count = staticExports->numArgs;
    m->export_entries_size = m->export_entries_count;
    m->export_entries = (JSExportEntry *)js_mallocz(
        ctx, sizeof(m->export_entries[0]) * m->export_entries_size);

    for (int ex = 0; ex < staticExports->numArgs; ++ex) {
      IridiumSEXP *staticExport = staticExports->args[ex];

      if (isTag(staticExport, "NamedReexport")) {
        // Module Request IDX
        int reqIdx = getFlagNumber(staticExport, "MODULEREQIDX");
        m->export_entries[ex].u.req_module_idx = reqIdx;
        m->export_entries[ex].export_type = JS_EXPORT_TYPE_INDIRECT;
        m->export_entries[ex].local_name =
            JS_NewAtom(ctx, getFlagString(staticExport, "LOCALNAME"));
        m->export_entries[ex].export_name =
            JS_NewAtom(ctx, getFlagString(staticExport, "EXPORTNAME"));
      } else {
        // Stack Ref IDX
        IridiumSEXP *bindingTarget = staticExport->args[0];
        ensureTag(bindingTarget, "RemoteEnvBinding");
        int bindingTargetIDX = getFlagNumber(bindingTarget, "REFIDX");

        m->export_entries[ex].u.local.var_idx = bindingTargetIDX;
        m->export_entries[ex].export_type = JS_EXPORT_TYPE_LOCAL;
        m->export_entries[ex].local_name =
            JS_NewAtom(ctx, getFlagString(staticExport, "LOCALNAME"));
        m->export_entries[ex].export_name =
            JS_NewAtom(ctx, getFlagString(staticExport, "EXPORTNAME"));
      }
    }
  }

  if (starExports->numArgs > 0) {
    // 4. Star Exports
    m->star_export_entries_count = starExports->numArgs;
    m->star_export_entries_size = m->star_export_entries_count;
    m->star_export_entries = (JSStarExportEntry *)js_mallocz(
        ctx, sizeof(m->star_export_entries[0]) * m->star_export_entries_size);

    for (int ex = 0; ex < starExports->numArgs; ++ex) {
      IridiumSEXP *starExport = starExports->args[ex];
      // Req IDX
      int reqIDX = getFlagNumber(starExport, "MODULEREQIDX");
      m->star_export_entries[ex].req_module_idx = reqIDX;
    }
  }

#ifdef LINKING_DUMP_FUNCTION
  if (check_dump_flag(ctx, JS_DUMP_BYTECODE_FINAL)) {
    fprintf(stdout, "[Iridium] Dumping compiled topLevel code\n");
    js_dump_function_bytecode(ctx, b);
  }
#endif

  m->func_obj = moduleFunVal;
  bool isTLA = hasFlag(iridiumCode, "TLA");
  m->has_tla = isTLA;

  return ((IridiumLoadResult){true, m});
}

void eval_iri_file(JSContext *ctx, const char *filename) {
  auto start = std::chrono::high_resolution_clock::now();
  json j; // No pointers, memory is managed automatically!
  {
    // ScopedTimer t("load_json");
    try {
      j = load_json(filename);
    } catch (const json::parse_error &e) {
      std::cerr << "Parse error at byte " << e.byte << ": " << e.what() << '\n';
      exit(1);
    } catch (const std::exception &e) {
      std::cerr << "File error: " << e.what() << '\n';
      exit(1);
    }
  }
  {
    IridiumLoadResult iriRes = compile_iri_module(ctx, j);

    // ScopedTimer t("evalIri");
    if (iriRes.isModule) {
      JSValue moduleVal = JS_NewModuleValue(ctx, (JSModuleDef *)iriRes.ptr);

      if (JS_ResolveModule(ctx, moduleVal) < 0) {
        JS_FreeValue(ctx, moduleVal);
        js_std_dump_error(ctx);
        exit(1);
      }

      auto end = std::chrono::high_resolution_clock::now();

      // Compute duration
      std::chrono::duration<double, std::milli> duration = end - start;
      std::cout << "[IRIDIUM] Parse time: " << duration.count() << " ms"
                << std::endl;

      JSValue res = JS_EvalFunction(ctx, moduleVal);

      end = std::chrono::high_resolution_clock::now();
      duration = end - start;
      std::cout << "[IRIDIUM] Execution time: " << duration.count() << " ms"
                << std::endl;

      JS_FreeValue(ctx, res);
    } else {
      JSValue func_val = JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, iriRes.ptr);

      auto end = std::chrono::high_resolution_clock::now();

      std::chrono::duration<double, std::milli> duration = end - start;
      std::cout << "[IRIDIUM] Parse time: " << duration.count() << " ms"
                << std::endl;
      start = std::chrono::high_resolution_clock::now();
      JSValue res = JS_EvalFunction(ctx, func_val);
      end = std::chrono::high_resolution_clock::now();
      duration = end - start;
      std::cout << "[IRIDIUM] Execution time: " << duration.count() << " ms"
                << std::endl;
      JS_FreeValue(ctx, res);
    }
  }
}

// Some basic bit operations
void setBit(int bitIndex, int *value) { *value |= (1 << bitIndex); }

void clearBit(int bitIndex, int *value) { *value &= ~(1 << bitIndex); }

void toggleBit(int bitIndex, int *value) { *value ^= (1 << bitIndex); }

bool isBitSet(int bitIndex, int value) {
  return (value & (1 << bitIndex)) != 0;
}
