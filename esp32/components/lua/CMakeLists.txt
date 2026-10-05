# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Lua 5.4.6 (MIT) for the apps Muse sends (CONFIG_MUSE_LUA). See README.md.
set(srcs)
if(CONFIG_MUSE_LUA)
    file(GLOB srcs "${COMPONENT_DIR}/src/*.c")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "src")
if(CONFIG_MUSE_LUA)
    # Upstream code, built as released: its warnings aren't ours to fix.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-error -w)
    # C calls (the parser's nesting among them) deep enough for any app, with
    # room on the runner's 32 KB stack.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LUAI_MAXCCALLS=80)
endif()
