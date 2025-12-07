#!/usr/bin/env python3
import json, shlex

with open("compile_commands.json") as f:
    data = json.load(f)

assert len(data) > 0

command = data[0]["command"]
import shlex

parts = shlex.split(command)
result = []
skip_next = False

for p in parts:
    if skip_next:
        skip_next = False
        continue
    if p in ("-o", "-c"):
        skip_next = True
        continue
    result.append(p)

result = [result[0]] + ["-fPIC", "-fvisibility=hidden"] + result[1:]
cleaned = " ".join(result)

with open("include/JITOptions.hpp", "w") as f:
    f.write("#pragma once\n")
    f.write("namespace engine::jit {\n")
    f.write(f'static constexpr const char* compileCommand = R"JITFLAGS({cleaned} )JITFLAGS";\n')
    f.write(f'static constexpr const char* linkCommand = R"JITFLAGS({result[0]} -shared -fPIC )JITFLAGS";\n')
    f.write("}\n")
