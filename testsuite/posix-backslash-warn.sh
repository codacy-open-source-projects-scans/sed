#!/bin/sh
# Test for warnings using backslashed characters in the 's' command.

# Copyright (C) 2025-2026 Free Software Foundation, Inc.

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

. "${srcdir=.}/testsuite/init.sh"; path_prepend_ ./sed
print_ver_ sed

# Test that '\n' emits a warning.
echo ab > input1 || framework_failure_
printf 'a\nb\n' > exp-out1 || framework_failure_
cat << \EOF > exp-err1 || framework_failure_
sed: warning: using "\n" in the 's' command is not portable
EOF

sed --posix 's/ab/a\nb/g' input1 > out1 2> out-err1 || fail=1
compare_ out1 exp-out1 || fail=1
compare_ out-err1 exp-err1 || fail=1

# Test that '\1' does not emit a warning.
echo abc > input2 || framework_failure_
echo abc > exp-out2 || framework_failure_

sed --posix 's/(abc)/\1/g' input2 > out2 2> out-err2 || fail=1
compare_ out2 exp-out2 || fail=1
compare_ out-err2 /dev/null || fail=1

# Test that '\&' does not emit a warning.
echo abc > input3 || framework_failure_
echo '&' > exp-out3 || framework_failure_

sed --posix 's/abc/\&/g' input3 > out3 2> out-err3 || fail=1
compare_ out3 exp-out3 || fail=1
compare_ out-err3 /dev/null || fail=1

# Test that '\\' does not emit a warning.
echo abc > input4 || framework_failure_
printf '\\\n' > exp-out4 || framework_failure_

sed --posix 's/abc/\\/g' input4 > out4 2> out-err4 || fail=1
compare_ out4 exp-out4 || fail=1
compare_ out-err4 /dev/null || fail=1

# Test that '\|' does not emit a warning when '|' is a delimiter.
echo abc > input5 || framework_failure_
echo '|' > exp-out5 || framework_failure_

sed --posix 's|abc|\||g' input5 > out5 2> out-err5 || fail=1
compare_ out5 exp-out5 || fail=1
compare_ out-err5 /dev/null || fail=1

# Test that '\|' does emit a warning when '|' is a not delimiter.
echo abc > input6 || framework_failure_
echo '|' > exp-out6 || framework_failure_
cat << \EOF > exp-err6 || framework_failure_
sed: warning: using "\|" in the 's' command is not portable
EOF

sed --posix 's/abc/\|/g' input6 > out6 2> out-err6 || fail=1
compare_ out6 exp-out6 || fail=1
compare_ out-err6 exp-err6 || fail=1

# Test that BRE interval expressions do not warn.
echo abc > input7 || framework_failure_
echo def > exp-out7 || framework_failure_

sed --posix 's/abc\{0,1\}/def/g' input7 > out7 2> out-err7 || fail=1
compare_ out7 exp-out7 || fail=1
compare_ out-err7 /dev/null || fail=1

# Test that BRE subexpressions do not warn.
echo abc > input8 || framework_failure_
echo def > exp-out8 || framework_failure_

sed --posix 's/ab\(c\)/def/g' input8 > out8 2> out-err8 || fail=1
compare_ out8 exp-out8 || fail=1
compare_ out-err8 /dev/null || fail=1

# Test that ERE special characters do not warn.
echo '.[\()*+?{}|^$' > input9
echo abc > exp-out9 || framework_failure_

sed -E --posix 's/^\.\[\\\(\)\*\+\?\{\}\|\^\$$/abc/g' \
  input9 > out9 2> out-err9 || fail=1
compare_ out9 exp-out9 || fail=1
compare_ out-err9 /dev/null || fail=1

Exit $fail
