#version 450
// nfs35 (Switch): the grass counters without transfer commands. Same set 0 layout as detail_generate.comp (bindings 4 and
// 5 only) and the same pipeline layout; push = (mode, numObjTypes). mode 0: zero every counter (used [0..63], dropped
// [64..127], spare) -- replaces vkCmdFillBuffer; mode 1: indirect[i].instanceCount = counters[i] -- replaces the
// 21-region vkCmdCopyBuffer. One workgroup.
layout(local_size_x = 64) in;
layout(set = 0, binding = 4) buffer AtomicCounters   { uint counters[]; } atomics;
layout(set = 0, binding = 5) buffer IndirectCommands { uint cmds[]; } indirect;
layout(push_constant) uniform P { uint mode; uint numObjTypes; } p;
void main()
{
    const uint t = gl_LocalInvocationID.x;
    if (p.mode == 0u) {
        for (uint i = t; i < 132u; i += 64u) atomics.counters[i] = 0u;
    } else if (t < p.numObjTypes) {
        indirect.cmds[t * 5u + 1u] = atomics.counters[t];   // VkDrawIndexedIndirectCommand = 5 uints, instanceCount at 1
    }
}
