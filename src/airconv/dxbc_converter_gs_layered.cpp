/*
 * Layered geometry shader: run a simple "send each triangle to N array
 * slices" geometry shader as part of the vertex shader, with instancing.
 *
 * The general geometry shader path (dxbc_converter_gs.cpp) emulates the GS
 * with object and mesh shaders. Each GS invocation (one triangle) runs in its
 * own one-thread mesh threadgroup, so the GPU runs that stage with 1 of 32
 * SIMD lanes active. Cube-map shadow passes (for example UE4's one-pass
 * point light shadows) spend most of their time there.
 *
 * Many such geometry shaders have this shape:
 *
 *   for each face f (unrolled):
 *     if (uniform condition, for example "mesh visible in face f")
 *       compute clip positions of the 3 input vertices for face f
 *       if (per-triangle frustum test passes)
 *         emit vertex 0, 1, 2 with SV_RenderTargetArrayIndex = f; cut
 *
 * The output of each emitted vertex depends only on its own input vertex and
 * on constants. So the same work can run per vertex: draw N instances, let
 * instance i handle face (i % N), and write the face index to
 * [[render_target_array_index]] from the vertex function.
 *
 * Two parts of the GS change meaning in this form:
 * - A branch whose condition depends on vertex data (the per-triangle frustum
 *   test) cannot run per vertex, so it is always taken. This is only safe for
 *   culling tests: the rasterizer clips the triangles that the test would
 *   have removed. The analysis below accepts such a branch only when it has
 *   no else part, so taking it can only add a triangle, never change one.
 * - A branch whose condition depends only on constants runs as written,
 *   because it has the same result for all 3 vertices of a triangle.
 *
 * analyze_dxbc_layered_geometry_shader() proves the shape with a data-flow
 * pass over the DXBC control flow graph. Anything it does not understand
 * makes the shader fall back to the general path.
 */

#include "airconv_error.hpp"
#include "dxbc_converter.hpp"
#include "dxil_converter.hpp"
#include "nt/air_builder.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace dxmt::dxbc {

using namespace microsoft;

namespace {

template <class... Ts> struct layered_visitor : Ts... {
  using Ts::operator()...;
};
template <class... Ts> layered_visitor(Ts...) -> layered_visitor<Ts...>;

/* What one register component depends on. */
struct Taint {
  /* bit j: the value depends on input vertex j of the triangle */
  uint8_t inputs = 0;
  /* bit r: the value was written inside the body of forced branch r */
  uint64_t regions = 0;

  Taint operator|(const Taint &o) const { return {uint8_t(inputs | o.inputs), regions | o.regions}; }
  Taint &operator|=(const Taint &o) { return *this = *this | o; }
};

using Taint4 = std::array<Taint, 4>;
using Literal4 = std::array<std::optional<uint32_t>, 4>;

struct State {
  std::vector<Taint4> temps;
  std::vector<Taint4> outputs;
  /* known constant value of an output component (only through `mov o, l(x)`) */
  std::vector<Literal4> output_literals;

  void merge(const State &o) {
    for (size_t i = 0; i < temps.size(); i++)
      for (int c = 0; c < 4; c++)
        temps[i][c] |= o.temps[i][c];
    for (size_t i = 0; i < outputs.size(); i++)
      for (int c = 0; c < 4; c++) {
        outputs[i][c] |= o.outputs[i][c];
        if (output_literals[i][c] != o.output_literals[i][c])
          output_literals[i][c] = std::nullopt;
      }
  }
};

class LayeredAnalysis {
public:
  LayeredAnalysis(const SM50ShaderInternal *gs, uint32_t rtai_reg, uint32_t rtai_comp) :
      gs_(gs),
      rtai_reg_(rtai_reg),
      rtai_comp_(rtai_comp) {}

  /* Returns the face count, or 0 if the shader does not qualify. */
  uint32_t
  run() {
    BasicBlock *entry = gs_->entry();

    /* Collect reachable blocks and count incoming edges. */
    std::vector<BasicBlock *> stack{entry};
    std::unordered_set<BasicBlock *> reachable{entry};
    std::unordered_map<BasicBlock *, uint32_t> indegree;
    while (!stack.empty()) {
      auto bb = stack.back();
      stack.pop_back();
      for (auto succ : successors(bb)) {
        if (!succ)
          return 0;
        indegree[succ]++;
        if (reachable.insert(succ).second)
          stack.push_back(succ);
      }
    }

    State initial;
    initial.temps.resize(gs_->shader_info.tempRegisterCount);
    initial.outputs.resize(gs_->max_output_register);
    initial.output_literals.resize(gs_->max_output_register);
    in_state_[entry] = initial;

    /* Visit the blocks in topological order. A cycle (a loop) leaves blocks
       unvisited, and the shader does not qualify. */
    std::queue<BasicBlock *> ready;
    ready.push(entry);
    size_t visited = 0;
    while (!ready.empty()) {
      auto bb = ready.front();
      ready.pop();
      visited++;
      State state = in_state_[bb];
      if (!visit_block(bb, state))
        return 0;
      for (auto succ : successors(bb)) {
        if (auto it = in_state_.find(succ); it != in_state_.end())
          it->second.merge(state);
        else
          in_state_.emplace(succ, state);
        if (--indegree[succ] == 0)
          ready.push(succ);
      }
    }
    if (visited != reachable.size())
      return 0;

    /* The faces must be exactly 0, 1, ..., N-1, each emitted once. */
    uint32_t faces = faces_.size();
    if (faces == 0 || faces > 8)
      return 0;
    for (uint32_t f = 0; f < faces; f++)
      if (!faces_.contains(f))
        return 0;
    return faces;
  }

  std::unordered_set<const BasicBlock *> forced;

private:
  const SM50ShaderInternal *gs_;
  uint32_t rtai_reg_;
  uint32_t rtai_comp_;
  std::unordered_map<BasicBlock *, State> in_state_;
  /* forced branch regions that enclose each block */
  std::unordered_map<const BasicBlock *, uint64_t> enclosing_;
  std::set<uint32_t> faces_;

  /* per-block emit state */
  uint32_t strip_vertices_ = 0;
  uint32_t strip_face_ = 0;
  uint64_t current_regions_ = 0;

  static std::vector<BasicBlock *>
  successors(BasicBlock *bb) {
    return std::visit(
        layered_visitor{
            [](const BasicBlockUnconditionalBranch &b) { return std::vector<BasicBlock *>{b.target}; },
            [](const BasicBlockConditionalBranch &b) {
              return std::vector<BasicBlock *>{b.true_branch, b.false_branch};
            },
            [](const BasicBlockReturn &) { return std::vector<BasicBlock *>{}; },
            /* unsupported terminators are rejected in visit_block() */
            [](const auto &) { return std::vector<BasicBlock *>{}; },
        },
        bb->target
    );
  }

  /* Blocks reachable from `from` without passing through `stop`. */
  static std::unordered_set<BasicBlock *>
  region_body(BasicBlock *from, BasicBlock *stop, bool &reaches_stop) {
    std::unordered_set<BasicBlock *> body;
    std::vector<BasicBlock *> stack{from};
    reaches_stop = false;
    while (!stack.empty()) {
      auto bb = stack.back();
      stack.pop_back();
      if (bb == stop) {
        reaches_stop = true;
        continue;
      }
      if (!body.insert(bb).second)
        continue;
      for (auto succ : successors(bb))
        stack.push_back(succ);
    }
    return body;
  }

  /* Taint of each component of a source operand, after its swizzle. */
  std::optional<Taint4>
  read(const SrcOperand &src, const State &state, Literal4 *literal = nullptr) {
    Taint4 result{};
    if (literal)
      *literal = Literal4{};
    bool ok = std::visit(
        layered_visitor{
            [&](const SrcOperandImmediate32 &imm) {
              Swizzle swizzle = imm._.swizzle;
              if (literal && !imm._.abs && !imm._.neg)
                for (int c = 0; c < 4; c++)
                  (*literal)[c] = imm.uvalue[swizzle[c]];
              return true;
            },
            [&](const SrcOperandTemp &temp) {
              if (temp.regid >= state.temps.size())
                return false;
              Swizzle swizzle = temp._.swizzle;
              for (int c = 0; c < 4; c++)
                result[c] = state.temps[temp.regid][swizzle[c]];
              return true;
            },
            [&](const SrcOperandInputICP &input) {
              auto cpid = std::get_if<uint32_t>(&input.cpid);
              auto regid = std::get_if<uint32_t>(&input.regid);
              if (!cpid || !regid || *cpid >= 3)
                return false;
              for (int c = 0; c < 4; c++)
                result[c].inputs = uint8_t(1u << *cpid);
              return true;
            },
            [&](const SrcOperandConstantBuffer &cb) {
              return std::holds_alternative<uint32_t>(cb.rangeindex) && std::holds_alternative<uint32_t>(cb.regindex);
            },
            [&](const SrcOperandImmediateConstantBuffer &icb) {
              return std::holds_alternative<uint32_t>(icb.regindex);
            },
            /* indexable temps, other inputs, system values: not supported */
            [](const auto &) { return false; },
        },
        src
    );
    if (!ok)
      return std::nullopt;
    return result;
  }

  bool
  write(const DstOperand &dst, const Taint4 &value, State &state, const Literal4 *literal = nullptr) {
    return std::visit(
        layered_visitor{
            [](const DstOperandNull &) { return true; },
            [&](const DstOperandTemp &temp) {
              if (temp.regid >= state.temps.size())
                return false;
              for (int c = 0; c < 4; c++)
                if (temp._.mask & (1 << c))
                  state.temps[temp.regid][c] = value[c] | Taint{0, current_regions_};
              return true;
            },
            [&](const DstOperandOutput &out) {
              if (out.regid >= state.outputs.size())
                return false;
              for (int c = 0; c < 4; c++)
                if (out._.mask & (1 << c)) {
                  state.outputs[out.regid][c] = value[c] | Taint{0, current_regions_};
                  state.output_literals[out.regid][c] = literal ? (*literal)[c] : std::nullopt;
                }
              return true;
            },
            [](const auto &) { return false; },
        },
        dst
    );
  }

  /* dst[c] = union of srcs[c] */
  template <typename... Srcs>
  bool
  componentwise(State &state, const DstOperand &dst, const Srcs &...srcs) {
    Taint4 value{};
    for (const SrcOperand *src : {&srcs...}) {
      auto t = read(*src, state);
      if (!t)
        return false;
      for (int c = 0; c < 4; c++)
        value[c] |= (*t)[c];
    }
    return write(dst, value, state);
  }

  bool
  emit(State &state) {
    uint32_t corner = strip_vertices_;
    if (corner >= 3)
      return false;
    /* Each emitted value may depend only on input vertex `corner` and on
       work that also runs whenever this emit runs. */
    for (auto &reg : state.outputs)
      for (auto &t : reg) {
        if (t.inputs & ~(1u << corner))
          return false;
        if (t.regions & ~current_regions_)
          return false;
      }
    auto face = state.output_literals[rtai_reg_][rtai_comp_];
    if (!face)
      return false;
    if (corner == 0)
      strip_face_ = *face;
    else if (strip_face_ != *face)
      return false;
    strip_vertices_++;
    return true;
  }

  bool
  cut() {
    if (strip_vertices_ == 0)
      return true;
    if (strip_vertices_ != 3)
      return false;
    if (!faces_.insert(strip_face_).second)
      return false;
    strip_vertices_ = 0;
    return true;
  }

  template <typename T>
  bool
  visit_instruction(const T &inst, State &state);

  bool
  visit_block(BasicBlock *bb, State &state) {
    current_regions_ = enclosing_[bb];
    strip_vertices_ = 0;

    bool ok = true;
    bb->instructions.for_each([&](const auto &inst) {
      if (ok)
        ok = visit_instruction(inst, state);
    });
    if (!ok)
      return false;
    /* Every strip must end with a cut in the same block. */
    if (strip_vertices_ != 0)
      return false;

    return std::visit(
        layered_visitor{
            [&](const BasicBlockReturn &) {
              /* A return inside a forced body would skip later faces. */
              return current_regions_ == 0;
            },
            [](const BasicBlockUnconditionalBranch &) { return true; },
            [&](const BasicBlockConditionalBranch &branch) {
              auto t = read(branch.cond.operand, state);
              if (!t)
                return false;
              Taint cond = (*t)[0];
              /* The condition must not use work that may be skipped here. */
              if (cond.regions & ~current_regions_)
                return false;
              if (cond.inputs == 0)
                return true; // same result for all vertices: keep the branch
              /* Vertex-dependent condition: always take the true branch.
                 Allow only `if` without `else`: the false target must be
                 the join point of the true body. */
              if (branch.true_branch == branch.false_branch)
                return false;
              bool reaches_join = false;
              auto body = region_body(branch.true_branch, branch.false_branch, reaches_join);
              if (!reaches_join)
                return false;
              uint32_t region = forced.size();
              if (region >= 64)
                return false;
              for (auto body_bb : body)
                enclosing_[body_bb] |= uint64_t(1) << region;
              forced.insert(bb);
              return true;
            },
            [](const auto &) { return false; },
        },
        bb->target
    );
  }
};

template <typename T>
bool
LayeredAnalysis::visit_instruction(const T &inst, State &state) {
        if constexpr (std::is_same_v<T, InstNop>) {
          return true;
        } else if constexpr (std::is_same_v<T, InstEmit>) {
          return emit(state);
        } else if constexpr (std::is_same_v<T, InstCut>) {
          return cut();
        } else if constexpr (std::is_same_v<T, InstMov>) {
          Literal4 literal;
          auto t = read(inst.src, state, &literal);
          if (!t)
            return false;
          bool keep_literal = !inst._.saturate;
          return write(inst.dst, *t, state, keep_literal ? &literal : nullptr);
        } else if constexpr (std::is_same_v<T, InstMovConditional>) {
          return componentwise(state, inst.dst, inst.src_cond, inst.src0, inst.src1);
        } else if constexpr (std::is_same_v<T, InstDotProduct>) {
          auto t0 = read(inst.src0, state);
          auto t1 = read(inst.src1, state);
          if (!t0 || !t1 || inst.dimension > 4)
            return false;
          Taint all{};
          for (int c = 0; c < inst.dimension; c++)
            all |= (*t0)[c] | (*t1)[c];
          return write(inst.dst, Taint4{all, all, all, all}, state);
        } else if constexpr (std::is_same_v<T, InstSinCos>) {
          return componentwise(state, inst.dst_sin, inst.src) && componentwise(state, inst.dst_cos, inst.src);
        } else if constexpr (std::is_same_v<T, InstConvert> || std::is_same_v<T, InstFloatUnaryOp> ||
                             std::is_same_v<T, InstIntegerUnaryOp>) {
          return componentwise(state, inst.dst, inst.src);
        } else if constexpr (std::is_same_v<T, InstIntegerCompare> || std::is_same_v<T, InstFloatCompare> ||
                             std::is_same_v<T, InstFloatBinaryOp> || std::is_same_v<T, InstIntegerBinaryOp>) {
          return componentwise(state, inst.dst, inst.src0, inst.src1);
        } else if constexpr (std::is_same_v<T, InstIntegerBinaryOpWithTwoDst>) {
          return componentwise(state, inst.dst_hi, inst.src0, inst.src1) &&
                 componentwise(state, inst.dst_low, inst.src0, inst.src1);
        } else if constexpr (std::is_same_v<T, InstFloatMAD> || std::is_same_v<T, InstIntegerMAD> ||
                             std::is_same_v<T, InstMaskedSumOfAbsDiff> || std::is_same_v<T, InstExtractBits>) {
          return componentwise(state, inst.dst, inst.src0, inst.src1, inst.src2);
        } else if constexpr (std::is_same_v<T, InstBitFiledInsert>) {
          return componentwise(state, inst.dst, inst.src0, inst.src1, inst.src2, inst.src3);
        } else {
          /* resource access, derivatives, atomics, swapc, ...: not supported */
          return false;
        }
}

} // namespace

uint32_t
analyze_dxbc_layered_geometry_shader(SM50ShaderInternal *gs, CSignatureParser5 &outputParser) {
  gs->gs_layered_faces = 0;
  gs->gs_layered_forced_branches.clear();

  if (gs->dxil)
    return 0;
  if (gs->gs_input_primitive != D3D10_SB_PRIMITIVE_TRIANGLE)
    return 0;
  if (gs->gs_output_topology != D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
    return 0;
  if (gs->gs_instance_count > 1)
    return 0;
  if (outputParser.NumStreams() > 1 || outputParser.RasterizedStream())
    return 0;
  auto &info = gs->shader_info;
  if (!info.srvMap.empty() || !info.uavMap.empty() || !info.samplerMap.empty() || !info.tgsmMap.empty() ||
      !info.indexableTempRegisterCounts.empty())
    return 0;
  if (!gs->clip_distance_scalars.empty() || !gs->cull_distance_scalars.empty())
    return 0;

  uint32_t rtai_reg = ~0u, rtai_comp = 0;
  bool has_position = false;
  for (auto &element : gs->output_signature) {
    if (element.stream() != 0)
      return 0;
    switch (element.systemValue()) {
    case D3D10_SB_NAME_UNDEFINED:
      break;
    case D3D10_SB_NAME_POSITION:
      has_position = true;
      break;
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX:
      if (rtai_reg != ~0u || std::popcount(element.mask() & 0xFu) != 1)
        return 0;
      rtai_reg = element.reg();
      rtai_comp = std::countr_zero(element.mask() & 0xFu);
      break;
    default:
      return 0;
    }
  }
  if (!has_position || rtai_reg == ~0u || rtai_reg >= gs->max_output_register)
    return 0;

  LayeredAnalysis analysis(gs, rtai_reg, rtai_comp);
  uint32_t faces = analysis.run();
  if (!faces)
    return 0;
  gs->gs_layered_faces = faces;
  gs->gs_layered_forced_branches = std::move(analysis.forced);
  return faces;
}

llvm::Error
convert_dxbc_vertex_for_layered_geometry_shader(
    const SM50ShaderInternal *pVertexStage, const char *name, const SM50ShaderInternal *pGeometryStage,
    llvm::LLVMContext &context, llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  const uint32_t faces = pGeometryStage->gs_layered_faces;
  if (!faces)
    return llvm::make_error<UnsupportedFeature>("geometry shader is not layered");

  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(pArgs, &ia_layout);
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  if (args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig))
    return llvm::make_error<UnsupportedFeature>("layered geometry shader with root signature");

  auto vs_info = &pVertexStage->shader_info;
  auto gs_info = &pGeometryStage->shader_info;
  const uint32_t vs_max_input = pVertexStage->max_input_register;
  const uint32_t vs_max_output = pVertexStage->max_output_register;
  const uint32_t gs_max_output = pGeometryStage->max_output_register;

  // The function takes the VS inputs and returns the GS outputs.
  auto func_signature = pVertexStage->func_signature; // copy
  func_signature.ClearOutputs();

  Prologues prologue_vs;
  Epilogues epilogue_vs_unused;
  Epilogues epilogue_out;
  io_binding_map res_vs;
  io_binding_map res_gs;
  io_binding_map res_out;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue_vs, epilogue_vs_unused, func_signature, res_vs);
    sig_ctx.ia_layout = ia_layout;
    sig_ctx.skip_vertex_output = true;
    for (auto &p : pVertexStage->signature_handlers) {
      p(sig_ctx);
    }
  }

  uint32_t position_reg = ~0u;
  uint32_t rtai_reg = ~0u, rtai_comp = 0;
  for (auto &element : pGeometryStage->output_signature) {
    auto reg = element.reg();
    auto mask = element.mask() & 0xFu;
    switch (element.systemValue()) {
    case D3D10_SB_NAME_POSITION: {
      position_reg = reg;
      auto index = func_signature.DefineOutput(air::OutputPosition{.type = air::msl_float4});
      epilogue_out.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
        return io.PopulateOutputSanitizePosition(reg, mask, ret, index);
      });
      break;
    }
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX: {
      rtai_reg = reg;
      rtai_comp = std::countr_zero(mask);
      auto index = func_signature.DefineOutput(air::OutputRenderTargetArrayIndex{});
      epilogue_out.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
        return io.PopulateOutput(reg, mask, ret, index);
      });
      break;
    }
    case D3D10_SB_NAME_UNDEFINED: {
      auto index = func_signature.DefineOutput(air::OutputVertex{
          .user = element.consistentAttributeName(),
          .type = to_msl_type(element.componentType()),
      });
      epilogue_out.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
        return io.PopulateOutput(reg, mask, ret, index);
      });
      break;
    }
    default:
      return llvm::make_error<UnsupportedFeature>("unexpected system value in layered geometry shader");
    }
  }
  if (position_reg == ~0u || rtai_reg == ~0u)
    return llvm::make_error<UnsupportedFeature>("layered geometry shader without position or array index");

  uint32_t vertex_idx = func_signature.DefineInput(air::InputVertexID{});
  uint32_t base_vertex_idx = func_signature.DefineInput(air::InputBaseVertex{});
  uint32_t instance_idx = func_signature.DefineInput(air::InputInstanceID{});
  uint32_t base_instance_idx = func_signature.DefineInput(air::InputBaseInstance{});

  // VS resources use the usual vertex stage slots, GS resources the second set.
  auto vs_binding = setup_binding_table2(vs_info, func_signature, module);
  auto gs_binding = setup_binding_table2(
      gs_info, func_signature, module, SM50_BINDING_INDEX_CONSTANT_BUFFER2, SM50_BINDING_INDEX_ARGUMENT_TABLE2
  );

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, false);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto vs_end_bb = llvm::BasicBlock::Create(context, "vs_end", function);
  auto gs_end_bb = llvm::BasicBlock::Create(context, "gs_end", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );
  setup_metal_version(module, metal_version);

  /* Metal instance i = D3D instance (i / faces), face (i % faces). The draw
     multiplies the base instance by `faces` too. */
  auto faces_const = builder.getInt32(faces);
  auto metal_instance = function->getArg(instance_idx);
  auto metal_base_instance = function->getArg(base_instance_idx);
  auto local_instance = builder.CreateSub(metal_instance, metal_base_instance);
  auto face = builder.CreateURem(local_instance, faces_const);

  res_vs.vertex_id_with_base = function->getArg(vertex_idx);
  res_vs.base_vertex_id = function->getArg(base_vertex_idx);
  res_vs.vertex_id = builder.CreateSub(res_vs.vertex_id_with_base, res_vs.base_vertex_id);
  res_vs.instance_id = builder.CreateUDiv(local_instance, faces_const);
  res_vs.base_instance_id = builder.CreateUDiv(metal_base_instance, faces_const);
  res_vs.instance_id_with_base = builder.CreateAdd(res_vs.instance_id, res_vs.base_instance_id);

  // All register files are allocas in the entry block, so LLVM can promote them.
  res_vs.input.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, vs_max_input));
  res_vs.input.ptr_float4 = builder.CreateBitCast(
      res_vs.input.ptr_int4, llvm::ArrayType::get(types._float4, vs_max_input)->getPointerTo()
  );
  res_vs.input_element_count = vs_max_input;
  auto vs_output_type = llvm::ArrayType::get(types._int4, vs_max_output);
  res_vs.output.ptr_int4 = builder.CreateAlloca(vs_output_type);
  res_vs.output.ptr_float4 = builder.CreateBitCast(
      res_vs.output.ptr_int4, llvm::ArrayType::get(types._float4, vs_max_output)->getPointerTo()
  );
  res_vs.output_element_count = vs_max_output;

  // GS input: 3 vertex slots. All 3 hold this vertex (see the file comment).
  auto gs_input_type = llvm::ArrayType::get(vs_output_type, 3);
  res_gs.input.ptr_int4 = builder.CreateAlloca(gs_input_type);
  res_gs.input.ptr_float4 = builder.CreateBitCast(
      res_gs.input.ptr_int4,
      llvm::ArrayType::get(llvm::ArrayType::get(types._float4, vs_max_output), 3)->getPointerTo()
  );
  res_gs.input_element_count = vs_max_output;
  auto gs_output_type = llvm::ArrayType::get(types._int4, gs_max_output);
  res_gs.output.ptr_int4 = builder.CreateAlloca(gs_output_type);
  res_gs.output.ptr_float4 = builder.CreateBitCast(
      res_gs.output.ptr_int4, llvm::ArrayType::get(types._float4, gs_max_output)->getPointerTo()
  );
  res_gs.output_element_count = gs_max_output;
  res_gs.gs_instance_id = builder.getInt32(0);

  /* The GS outputs of the first emit of this face. If the face emits
     nothing, the position stays outside the clip volume, so the triangle
     is culled. */
  res_out.output.ptr_int4 = builder.CreateAlloca(gs_output_type);
  res_out.output.ptr_float4 = builder.CreateBitCast(
      res_out.output.ptr_int4, llvm::ArrayType::get(types._float4, gs_max_output)->getPointerTo()
  );
  res_out.output_element_count = gs_max_output;
  builder.CreateStore(llvm::ConstantAggregateZero::get(gs_output_type), res_out.output.ptr_int4);
  builder.CreateStore(
      llvm::ConstantVector::get(
          {air.getFloat(2.0f), air.getFloat(2.0f), air.getFloat(2.0f), air.getFloat(1.0f)}
      ),
      builder.CreateConstInBoundsGEP2_32(
          llvm::ArrayType::get(types._float4, gs_max_output), res_out.output.ptr_float4, 0, position_reg
      )
  );
  auto captured = builder.CreateAlloca(types._int, nullptr, "captured");
  builder.CreateStore(builder.getInt32(0), captured);

  setup_temp_register(vs_info, res_vs, types, module, builder);
  setup_immediate_constant_buffer(vs_info, res_vs, types, module, builder);
  setup_temp_register(gs_info, res_gs, types, module, builder);
  setup_immediate_constant_buffer(gs_info, res_gs, types, module, builder);

  struct context ctx_vs{
      .builder = builder,
      .air = air,
      .binding = *vs_binding,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = res_vs,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pVertexStage->shader_type,
      .metal_version = metal_version,
  };

  IOHelper io_vs(air, function, types, res_vs);
  if (auto err = prologue_vs.Run(io_vs)) {
    return err;
  }

  // 1. The vertex shader.
  llvm::Expected<llvm::BasicBlock *> vs_entry =
      pVertexStage->dxil ? dxil::convert_basicblocks(pVertexStage->dxil_entry, ctx_vs, vs_end_bb)
                         : convert_basicblocks(pVertexStage->entry(), ctx_vs, vs_end_bb);
  if (auto err = vs_entry.takeError()) {
    return err;
  }
  builder.CreateBr(vs_entry.get());

  // 2. Copy the VS outputs into the 3 GS input slots.
  builder.SetInsertPoint(vs_end_bb);
  for (uint32_t slot = 0; slot < 3; slot++) {
    for (uint32_t reg = 0; reg < vs_max_output; reg++) {
      auto value = builder.CreateLoad(
          types._int4, builder.CreateConstInBoundsGEP2_32(vs_output_type, res_vs.output.ptr_int4, 0, reg)
      );
      builder.CreateStore(
          value, builder.CreateGEP(
                     gs_input_type, res_gs.input.ptr_int4,
                     {builder.getInt32(0), builder.getInt32(slot), builder.getInt32(reg)}
                 )
      );
    }
  }

  /* 3. The geometry shader, one copy per face. Copy k keeps only the emits
     whose array index is k, so LLVM can drop the work for other faces. */
  uint32_t copy_face = 0;
  res_gs.call_emit = [&]() {
    auto rtai = builder.CreateExtractElement(
        builder.CreateLoad(
            types._int4, builder.CreateConstInBoundsGEP2_32(gs_output_type, res_gs.output.ptr_int4, 0, rtai_reg)
        ),
        rtai_comp
    );
    auto take = builder.CreateAnd(
        builder.CreateICmpEQ(rtai, builder.getInt32(copy_face)),
        builder.CreateICmpEQ(builder.CreateLoad(types._int, captured), builder.getInt32(0))
    );
    for (uint32_t reg = 0; reg < gs_max_output; reg++) {
      auto src = builder.CreateConstInBoundsGEP2_32(gs_output_type, res_gs.output.ptr_int4, 0, reg);
      auto dst = builder.CreateConstInBoundsGEP2_32(gs_output_type, res_out.output.ptr_int4, 0, reg);
      builder.CreateStore(
          builder.CreateSelect(take, builder.CreateLoad(types._int4, src), builder.CreateLoad(types._int4, dst)),
          dst
      );
    }
    builder.CreateStore(
        builder.CreateOr(builder.CreateLoad(types._int, captured), builder.CreateZExt(take, types._int)), captured
    );
  };
  res_gs.call_cut = []() {};

  struct context ctx_gs{
      .builder = builder,
      .air = air,
      .binding = *gs_binding,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = res_gs,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pGeometryStage->shader_type,
      .metal_version = metal_version,
      .forced_true_branches = &pGeometryStage->gs_layered_forced_branches,
  };

  auto face_switch = builder.CreateSwitch(face, gs_end_bb, faces);
  for (uint32_t k = 0; k < faces; k++) {
    auto face_bb = llvm::BasicBlock::Create(context, "face", function);
    face_switch->addCase(builder.getInt32(k), face_bb);
    builder.SetInsertPoint(face_bb);
    copy_face = k;
    auto gs_entry = convert_basicblocks(pGeometryStage->entry(), ctx_gs, gs_end_bb);
    if (auto err = gs_entry.takeError()) {
      return err;
    }
    builder.CreateBr(gs_entry.get());
  }

  // 4. Return the captured GS outputs.
  builder.SetInsertPoint(gs_end_bb);
  IOHelper io_out(air, function, types, res_out);
  auto result = epilogue_out.Run(function->getReturnType(), io_out);
  if (auto err = result.takeError()) {
    return err;
  }
  builder.CreateRet(result.get());

  module.getOrInsertNamedMetadata("air.vertex")->addOperand(function_metadata);
  return llvm::Error::success();
}

} // namespace dxmt::dxbc
