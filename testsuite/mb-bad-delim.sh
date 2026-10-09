#!/bin/sh
# Test 's' and 'y' non-slash delimiters in multibyte locales

# Copyright (C) 2016-2026 Free Software Foundation, Inc.

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

require_en_utf8_locale_

# These tests use the following unicode character in various ways:
#   GREEK CAPITAL LETTER PHI (U+03A6)
#   UTF-8: hex: 0xCE     0xA6
#          oct: 0316     0246
#          bin: 11001110 10100110
#
# Octal encoding is used due to printf not supporting hex on older systems.
# Using the first octet alone (\316) causes various multibyte related functions
# to return '-2' (incomplete multibyte sequence).
# using the second octet alone (\246) causess same functions to return '-1'
# (invalid multibyte sequence).


# Allow a valid multibyte delimiter (instead of slash).
printf 's\316\246a\316\246b\316\246\n' > prog1 || framework_failure_
printf 'abracadabra\n' >in1 || framework_failure_
printf 'bbracadabra\n' >exp1 || framework_failure_

cat <<\EOF > exp-err1 || framework_failure_
sed: file prog1 line 1: delimiter character is not a single-byte character
EOF

env LC_ALL=en_US.UTF-8 sed -f prog1 <in1 >out1 2>err1 || fail=1
compare_ /dev/null err1 || fail=1
compare_ exp1 out1 || fail=1

# Allow an incomplete multibyte delimiter (instead of slash).
# This is an implementation-specific behavior.
printf 's\316a\316b\316\n' > prog2 || framework_failure_

env LC_ALL=en_US.UTF-8 sed -f prog2 <in1 >out2 2>err2 || fail=1
compare_ /dev/null err2 || fail=1
compare_ exp1 out2 || fail=1

# ... and accept octet \316 as delimiter in C locale.
LC_ALL=C sed -f prog2 <in1 >out2 2>err2 || fail=1
compare_ /dev/null err2 || fail=1
compare_ exp1 out2 || fail=1



# An invalid multibyte sequence is treated as a valid single byte,
# thus accepted as a delimiter (instead of slash).
# This is an implementation-specific behavior.
printf 's\246a\246b\246\n' > prog3 || framework_failure_
LC_ALL=en_US.UTF-8 sed -f prog3 <in1 >out3 2>err3 || fail=1
compare_ /dev/null err3 || fail=1
compare_ exp1 out3 || fail=1

# Expect identical result in C locale
LC_ALL=C sed -f prog3 <in1 >out4 2>err4 || fail=1
compare_ /dev/null err4 || fail=1
compare_ exp1 out4 || fail=1


Exit $fail
