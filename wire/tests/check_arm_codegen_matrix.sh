#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Paranoid compile/objdump matrix for scalar and protocol hot paths.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
DEFAULT_ARM_CXX="/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-g++.exe"
ARM_CXX="${ARM_CXX:-$DEFAULT_ARM_CXX}"
case "$ARM_CXX" in
	*g++.exe) DEFAULT_OBJDUMP="${ARM_CXX%g++.exe}objdump.exe" ;;
	*g++)     DEFAULT_OBJDUMP="${ARM_CXX%g++}objdump" ;;
	*)        DEFAULT_OBJDUMP="arm-none-eabi-objdump" ;;
esac
ARM_OBJDUMP="${ARM_OBJDUMP:-$DEFAULT_OBJDUMP}"
OUT="$HERE/out/codegen-matrix"
SCALAR_SOURCE="$HERE/arm_hotpath.cpp"
PROTOCOL_SOURCE="$HERE/protocol_hotpath.cpp"
COMMON="-c -std=gnu++20 \
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror \
	-fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -mthumb"
SCALAR_FUNCTIONS="
	wire_store_be16 wire_store_le16
	wire_store_be32 wire_store_le32
	wire_store_be64 wire_store_le64
	wire_store_be_float wire_store_le_float
	wire_load_be16 wire_load_le16
	wire_load_be32 wire_load_le32
	wire_load_be64 wire_load_le64
	wire_load_be_float wire_load_le_float
"
PROTOCOL_STRAIGHT_FUNCTIONS="
	cobs_store_length16 cobs_load_length16
	modbus_crc_store modbus_crc_load
	modbus_read_be32_exact modbus_read_le32_exact
"
CONTROL_FLOW='[[:space:]](b(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?(\.n|\.w)?|cbz|cbnz)[[:space:]]'
ASSEMBLY=""

if [ ! -x "$ARM_CXX" ]; then
	echo "ARM compiler not found: $ARM_CXX"
	exit 1
fi

mkdir -p "$OUT"

# Keep the focused Cortex-M7 instruction proof as the first gate.
sh "$HERE/check_arm_hotpath.sh"

function_body()
{
	awk "/<$1>:/,/^$/" "$ASSEMBLY"
}

forbid_instruction()
{
	name="$1"
	pattern="$2"
	description="$3"
	if function_body "$name" | grep -E "$pattern" >/dev/null; then
		echo "FAIL  $name: contains $description"
		function_body "$name"
		exit 1
	fi
}

require_instruction()
{
	name="$1"
	pattern="$2"
	description="$3"
	if ! function_body "$name" | grep -E "$pattern" >/dev/null; then
		echo "FAIL  $name: missing $description"
		function_body "$name"
		exit 1
	fi
}

inspect_straight_line()
{
	functions="$1"
	for name in $functions; do
		forbid_instruction "$name" '[[:space:]]blx?(\.n|\.w)?[[:space:]]' \
			'a helper call'
		forbid_instruction "$name" "$CONTROL_FLOW" 'runtime control flow'
	done
}

scalar_count=0
for cpu in cortex-m0 cortex-m0plus cortex-m3 cortex-m4 cortex-m7 \
		cortex-m23 cortex-m33 cortex-m55; do
	for optimization in -Os -O2 -O3; do
		optimization_name="${optimization#-}"
		for byte_order in little big; do
			for alignment in default strict; do
				unaligned_flag=""
				if [ "$alignment" = strict ]; then
					unaligned_flag="-mno-unaligned-access"
				fi
				stem="${cpu}_${optimization_name}_${byte_order}_${alignment}"
				object="$OUT/scalar_${stem}.o"
				assembly="$OUT/scalar_${stem}.asm"
				# shellcheck disable=SC2086
				"$ARM_CXX" $COMMON "$optimization" "-mcpu=$cpu" \
					"-m${byte_order}-endian" $unaligned_flag -I"$PROJ" \
					"$SCALAR_SOURCE" -o "$object"
				"$ARM_OBJDUMP" -d "$object" > "$assembly"
				ASSEMBLY="$assembly"
				inspect_straight_line "$SCALAR_FUNCTIONS"

				case "$cpu:$alignment" in
					cortex-m0:*|cortex-m0plus:*|cortex-m23:*|*:strict)
						require_instruction wire_store_be32 \
							'[[:space:]]strb(\.w)?[[:space:]]' \
							'alignment-safe byte stores'
						require_instruction wire_load_be32 \
							'[[:space:]]ldrb(\.w)?[[:space:]]' \
							'alignment-safe byte loads'
						;;
				esac
				scalar_count=$((scalar_count + 1))
			done
		done
	done
done

protocol_count=0
for cpu in cortex-m0 cortex-m4 cortex-m7 cortex-m33 cortex-m55; do
	for optimization in -Os -O2 -O3; do
		optimization_name="${optimization#-}"
		for byte_order in little big; do
			for alignment in default strict; do
				unaligned_flag=""
				if [ "$alignment" = strict ]; then
					unaligned_flag="-mno-unaligned-access"
				fi
				stem="${cpu}_${optimization_name}_${byte_order}_${alignment}"
				object="$OUT/protocol_${stem}.o"
				assembly="$OUT/protocol_${stem}.asm"
				# shellcheck disable=SC2086
				"$ARM_CXX" $COMMON "$optimization" "-mcpu=$cpu" \
					"-m${byte_order}-endian" $unaligned_flag -I"$PROJ" \
					"$PROTOCOL_SOURCE" -o "$object"
				"$ARM_OBJDUMP" -dr "$object" > "$assembly"
				ASSEMBLY="$assembly"
				inspect_straight_line "$PROTOCOL_STRAIGHT_FUNCTIONS"
				forbid_instruction modbus_crc_calculate \
					'[[:space:]]blx?(\.n|\.w)?[[:space:]]' 'a helper call'
				forbid_instruction modbus_crc_calculate \
					'_ZN3crc6detail6Engine.*lookup_E' 'a lookup-table reference'
				forbid_instruction modbus_crc_calculate_table \
					'[[:space:]]blx?(\.n|\.w)?[[:space:]]' 'a helper call'
				require_instruction modbus_crc_calculate_table \
					'[[:space:]]ldrh(\.w)?[[:space:]]' 'a 16-bit table load'
				require_instruction modbus_crc_calculate_table \
					'_ZN3crc6detail6Engine.*lookup_E' 'the CRC lookup table'
				protocol_count=$((protocol_count + 1))
			done
		done
	done
done

cobs_count=0
for cpu in cortex-m0 cortex-m4 cortex-m7 cortex-m33 cortex-m55; do
	for optimization in -Os -O2 -O3; do
		optimization_name="${optimization#-}"
		for source in "$PROJ/cobs/Decoder.cpp" "$PROJ/cobs/Encoder.cpp"; do
			name="$(basename "$source" .cpp)"
			object="$OUT/cobs_${name}_${cpu}_${optimization_name}.o"
			assembly="$OUT/cobs_${name}_${cpu}_${optimization_name}.asm"
			# shellcheck disable=SC2086
			"$ARM_CXX" $COMMON "$optimization" "-mcpu=$cpu" \
				-mlittle-endian -I"$PROJ/cobs" "$source" -o "$object"
			"$ARM_OBJDUMP" -dr "$object" > "$assembly"
			if [ "$name" = Decoder ]; then
				if grep -E '[[:space:]]blx?(\.n|\.w)?[[:space:]]' \
						"$assembly" >/dev/null; then
					echo "FAIL  $cpu $optimization Decoder contains a helper call"
					exit 1
				fi
			else
				# Cortex-M0-class constant /254 may legitimately use the
				# once-per-frame division helper. No other helper is allowed.
				if grep -E '[[:space:]]blx?(\.n|\.w)?[[:space:]]' "$assembly" |
						grep -v '__aeabi_uidiv' >/dev/null; then
					echo "FAIL  $cpu $optimization Encoder contains an unexpected helper"
					exit 1
				fi
			fi
			cobs_count=$((cobs_count + 1))
		done
	done
done

sh "$PROJ/modbus/rtu/tests/check_arm_crc_codegen.sh"
sh "$PROJ/crc/tests/check_arm_codegen.sh"

echo "ARM codegen matrix passed: $scalar_count scalar, $protocol_count protocol, $cobs_count COBS objects"
