#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Proves that unused table policies emit zero lookup bytes, while each selected
# width emits exactly one private read-only table of 256 entries.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
DEFAULT_ARM_CXX="/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-g++.exe"
ARM_CXX="${ARM_CXX:-$DEFAULT_ARM_CXX}"
case "$ARM_CXX" in
	*g++.exe)
		DEFAULT_ARM_NM="${ARM_CXX%g++.exe}nm.exe"
		DEFAULT_ARM_OBJDUMP="${ARM_CXX%g++.exe}objdump.exe"
		;;
	*g++)
		DEFAULT_ARM_NM="${ARM_CXX%g++}nm"
		DEFAULT_ARM_OBJDUMP="${ARM_CXX%g++}objdump"
		;;
	*)
		DEFAULT_ARM_NM="arm-none-eabi-nm"
		DEFAULT_ARM_OBJDUMP="arm-none-eabi-objdump"
		;;
esac
ARM_NM="${ARM_NM:-$DEFAULT_ARM_NM}"
ARM_OBJDUMP="${ARM_OBJDUMP:-$DEFAULT_ARM_OBJDUMP}"
OUT="$HERE/out/arm-codegen"
SOURCE="$HERE/crc_codegen.cpp"
COMMON="-c -std=gnu++20 -DNDEBUG \
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror \
	-fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
	-mthumb -mcpu=cortex-m7 -I$PROJ"

if [ ! -x "$ARM_CXX" ]; then
	echo "ARM compiler not found: $ARM_CXX"
	exit 1
fi

mkdir -p "$OUT"
count=0
bitwise_count=0
codec_count=0
CODEC_FUNCTIONS="
	crc_store_le8 crc_load_le8 crc_store_le16 crc_load_le16
	crc_store_le32 crc_load_le32 crc_store_le64 crc_load_le64
	crc_store_be8 crc_load_be8 crc_store_be16 crc_load_be16
	crc_store_be32 crc_load_be32 crc_store_be64 crc_load_be64"
for optimization in -Os -O2 -O3; do
	optimization_name="${optimization#-}"
	for byte_order in little big; do
		for alignment in default strict; do
			unaligned_flag=""
			if [ "$alignment" = strict ]; then
				unaligned_flag="-mno-unaligned-access"
			fi
			stem="${optimization_name}_${byte_order}_${alignment}"
			default_object="$OUT/bitwise_$stem.o"
			default_assembly="$OUT/bitwise_$stem.asm"
			# Merely naming every table class must emit no lookup.
			# shellcheck disable=SC2086
			"$ARM_CXX" $COMMON "$optimization" "-m${byte_order}-endian" \
				$unaligned_flag "$SOURCE" -o "$default_object"
			"$ARM_OBJDUMP" -dr "$default_object" > "$default_assembly"
			if "$ARM_NM" -S "$default_object" |
					grep -E 'Engine.*lookup_E$' >/dev/null; then
				echo "FAIL  unused table class emitted a lookup at $stem"
				exit 1
			fi
			for function in $CODEC_FUNCTIONS; do
				body="$(sed -n "/<$function>:/,/^$/p" "$default_assembly")"
				if [ -z "$body" ]; then
					echo "FAIL  missing codec probe $function at $stem"
					exit 1
				fi
				if echo "$body" |
						grep -E '[[:space:]](bl|blx|cbz|cbnz|beq|bne|bhi|bls|blo|bhs)(\.|[[:space:]])' \
						>/dev/null; then
					echo "FAIL  $function retained a call or runtime branch at $stem"
					echo "$body"
					exit 1
				fi
			done
			codec_count=$((codec_count + 1))

			no_crc_body="$(sed -n \
				'/<crc_no_crc_verify_probe>:/,/^$/p' "$default_assembly")"
			if echo "$no_crc_body" |
					grep -E '[[:space:]](ldr|str|bl|blx|cbz|cbnz|beq|bne|bhi|bls)(\.|[[:space:]])' \
					>/dev/null; then
				echo "FAIL  NoCrc verify retained memory access, call, or branch at $stem"
				echo "$no_crc_body"
				exit 1
			fi
			if ! echo "$no_crc_body" |
					grep -E '[[:space:]]mov(s|\.w)?[[:space:]]+r0,[[:space:]]*#1' \
					>/dev/null; then
				echo "FAIL  NoCrc verify did not fold to constant true at $stem"
				echo "$no_crc_body"
				exit 1
			fi
		done

		for width in 8 16 32 64; do
			case "$width" in
				8) expected=00000100 ;;
				16) expected=00000200 ;;
				32) expected=00000400 ;;
				64) expected=00000800 ;;
			esac
			stem="${optimization_name}_${byte_order}"
			bitwise_object="$OUT/bitwise_${width}_$stem.o"
			bitwise_assembly="$OUT/bitwise_${width}_$stem.asm"
			# shellcheck disable=SC2086
			"$ARM_CXX" $COMMON "$optimization" "-m${byte_order}-endian" \
				"-DCRC_BITWISE_${width}_PROBE=1" "$SOURCE" \
				-o "$bitwise_object"
			"$ARM_OBJDUMP" -dr "$bitwise_object" > "$bitwise_assembly"
			if "$ARM_NM" -S "$bitwise_object" |
					grep -E 'Engine.*lookup_E$' >/dev/null; then
				echo "FAIL  CRC$width Bitwise emitted a lookup at $stem"
				exit 1
			fi
			bitwise_body="$(sed -n \
				'/<crc_policy_calculate_probe>:/,/^$/p' "$bitwise_assembly")"
			if [ -z "$bitwise_body" ] || echo "$bitwise_body" |
					grep -E '[[:space:]](bl|blx)(\.|[[:space:]])' >/dev/null; then
				echo "FAIL  CRC$width Bitwise retained a helper call at $stem"
				echo "$bitwise_body"
				exit 1
			fi
			bitwise_count=$((bitwise_count + 1))

			object="$OUT/table_${width}_$stem.o"
			symbols="$OUT/table_${width}_$stem.nm"
			assembly="$OUT/table_${width}_$stem.asm"
			# shellcheck disable=SC2086
			"$ARM_CXX" $COMMON "$optimization" "-m${byte_order}-endian" \
				"-DCRC_TABLE_${width}_PROBE=1" "$SOURCE" -o "$object"
			"$ARM_NM" -S --size-sort "$object" > "$symbols"
			"$ARM_OBJDUMP" -dr "$object" > "$assembly"
			table_body="$(sed -n \
				'/<crc_policy_calculate_probe>:/,/^$/p' "$assembly")"
			if [ -z "$table_body" ] || echo "$table_body" |
					grep -E '[[:space:]](bl|blx)(\.|[[:space:]])' >/dev/null; then
				echo "FAIL  CRC$width Table retained a helper call at $stem"
				echo "$table_body"
				exit 1
			fi
			matches="$(grep -Ec "$expected [VvRr] _ZN3crc6detail6Engine.*lookup_E$" \
				"$symbols" || true)"
			if [ "$matches" -ne 1 ]; then
				echo "FAIL  CRC$width Table did not emit one $expected-byte lookup"
				cat "$symbols"
				exit 1
			fi
			if ! "$ARM_OBJDUMP" -t "$object" |
					grep -E "[[:space:]]O[[:space:]]+\.rodata\.[^[:space:]]+[[:space:]]+$expected[[:space:]]+_ZN3crc6detail6Engine.*lookup_E$" \
					>/dev/null; then
				echo "FAIL  CRC$width lookup is not one private read-only class object"
				exit 1
			fi
			count=$((count + 1))
		done
	done
done

echo "ARM CRC codegen passed: $bitwise_count bitwise and $count table policies, $codec_count codec matrices; algorithms/codecs are helper-free; unused tables emit zero bytes; NoCrc folds away"
