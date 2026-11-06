#!/bin/bash
# Build fsr2/libfsr2gl.a: AMD FSR 2.2 (MIT, src/LICENSE_FSR2.txt) with the OpenGL backend by
# JuanDiegoMontoya (github.com/JuanDiegoMontoya/FidelityFX-FSR2-OpenGL), patched for GCC/Linux
# (src/ffx_linux.h). Shaders: gen_fsr2_shaders.py (needs glslangValidator) replaces FidelityFX_SC.exe.
set -e; cd "$(dirname "$0")"
[ -f gen/ffx_fsr2_rcas_pass_permutations.h ] || python3 gen_fsr2_shaders.py src/shaders gen
mkdir -p obj
for f in ffx_fsr2.cpp ffx_assert.cpp gl/ffx_fsr2_gl.cpp gl/shaders/ffx_fsr2_shaders_gl.cpp; do
  g++ -std=c++17 -O2 -fPIC -include src/ffx_linux.h -I gen -I src -I src/gl/external/glad/include -c src/$f -o obj/$(basename $f .cpp).o
done
ar rcs libfsr2gl.a obj/ffx_fsr2.o obj/ffx_assert.o obj/ffx_fsr2_gl.o obj/ffx_fsr2_shaders_gl.o
echo built libfsr2gl.a
