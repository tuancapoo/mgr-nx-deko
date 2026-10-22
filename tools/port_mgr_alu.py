"""Apply MGR's proven scalar TEMP/a0 hazard handling to the vendored reNut translator."""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
target = root / 'app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_recompiler.cpp'
original = root / 'shaders/XenosRecomp/shader_recompiler.cpp'
text = target.read_text()
old = original.read_text()

needle = '#ifdef RENUT_RECOMP\n    // laneMask selects vector operand lanes explicitly instead of following the write mask.'
text = text.replace(needle, '    std::string scalarOperandOverrides[7];\n\n' + needle, 1)
needle = '        {\n            size_t reg = 0;'
text = text.replace(needle, '        {\n            if (!scalarOperandOverrides[operand].empty()) return scalarOperandOverrides[operand];\n            size_t reg = 0;', 1)

start = text.index('    else if (instr.vectorOpcode >= AluVectorOpcode::MaxA)', text.index('void ShaderRecompiler::recompile(const AluInstruction&'))
end = text.index('\n    uint32_t vectorWriteMask', start)
text = text[:start] + '''    bool setsA0 = false;
    if (!(instr.vectorOpcode >= AluVectorOpcode::SetpEqPush && instr.vectorOpcode <= AluVectorOpcode::SetpGePush) &&
        instr.vectorOpcode >= AluVectorOpcode::MaxA)
    {
        indent();
        println("banjoNextA0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(VECTOR_0, 0b1000));
        setsA0 = true;
    }
''' + text[end:]

start = old.index('    bool scalarSnapshotScope = false;')
end = old.index('\n    if (vectorWriteMask != 0)', start)
hazards = old[start:end].replace('fmt::format("me_alu_scalar_{}_{}", markPredicate, operand)', 'fmt::format("mgr_alu_scalar_{}_{}", out.size(), operand)')
hazards = hazards.replace('println("{}float {} = {};", precisePosition ? "precise " : "",\n                    scalarOperandOverrides[operand], expression);', 'println("float {} = {};", scalarOperandOverrides[operand], expression);')
needle = '\n#ifdef REBLUE_RECOMP\n    // A Sgt/Sge whose sources are ShadowTexture taps'
text = text.replace(needle, '\n' + hazards + needle, 1)

needle = '#ifdef RENUT_RECOMP\n    const size_t scalarBlockStart = out.size();\n#endif'
text = text.replace(needle, '''    if (setsA0)
    {
        indent();
        out += "a0 = banjoNextA0;\\n";
    }''', 1)
start = text.index('    #ifdef RENUT_RECOMP\n        // Both halves of an ALU instruction')
end = text.index('    #endif', start) + len('    #endif')
text = text[:start] + text[end:]

needle = '\n    if (closeIfBracket)'
text = text.replace(needle, '''
    if (scalarSnapshotScope)
    {
        --indentation;
        indent();
        out += "}\\n";
    }
''' + needle, 1)
text = text.replace('    out += "\\tint a0 = 0;\\n";', '    out += "\\tint a0 = 0;\\n";\n    out += "\\tint banjoNextA0 = 0;\\n";', 1)
target.write_text(text, newline='\n')
print('MGR scalar TEMP and vector a0 ordering ported')
