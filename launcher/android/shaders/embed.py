import struct

def to_c_array(path, varname):
    with open(path, 'rb') as f:
        data = f.read()
    assert len(data) % 4 == 0
    words = len(data) // 4
    lines = []
    lines.append(f"static const unsigned int {varname}[{words}] = {{")
    vals = struct.unpack(f"<{words}I", data)
    for i in range(0, len(vals), 8):
        chunk = vals[i:i+8]
        lines.append("\t" + ", ".join(f"0x{v:08x}" for v in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)

out = []
out.append("// Generated from basic.vert/basic.frag via glslc. Do not edit by hand -")
out.append("// regenerate with: glslc basic.vert -o basic.vert.spv && glslc basic.frag -o basic.frag.spv")
out.append("// then re-run shaders/embed.py")
out.append("#ifndef BASIC_SHADERS_SPV_H")
out.append("#define BASIC_SHADERS_SPV_H")
out.append("")
out.append(to_c_array("basic.vert.spv", "g_BasicVertSpv"))
out.append("")
out.append(to_c_array("basic.frag.spv", "g_BasicFragSpv"))
out.append("")
out.append("#endif")

with open("../basic_shaders_spv.h", "w") as f:
    f.write("\n".join(out) + "\n")

print("done")
