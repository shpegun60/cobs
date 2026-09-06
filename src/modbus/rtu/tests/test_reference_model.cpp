/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * reference_model.h, the data model the hardware comparison against QModbus
 * relies on from three sides. If it is wrong, two hardware tests lie in the
 * same direction, so it is checked here on the host first.
 */

#include "Test.h"
#include "reference_model.h"

#include <cstdint>
#include <span>
#include <vector>

using namespace modbus_test;
using namespace modbus_reference;

namespace {

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> list) { return std::vector<uint8_t>(list); }

Reply ask(Model& model, const uint8_t address, const uint8_t function, const std::vector<uint8_t>& data)
{
	return serve(model, kBoardUnit, address, function, std::span<const uint8_t>{data});
}

} // namespace

int main()
{
	Model model;
	model.reset();

	group("InitialContents");
	check(model.holding[0] == 0x1000u && model.holding[255] == 0x10FFu, "holding registers are 0x1000 + index");
	check(model.input[7] == 0x2007u, "input registers are 0x2000 + index");
	check(Model::bit(model.coils, 0) && !Model::bit(model.coils, 1) && Model::bit(model.coils, 3),
	      "every third coil is on");
	check(Model::bit(model.discrete, 0) && !Model::bit(model.discrete, 1), "every second discrete input is on");

	group("Reads");
	{
		const Reply r = ask(model, kBoardUnit, 0x03u, bytes({0x00u, 0x05u, 0x00u, 0x02u}));
		check(r.respond && r.function == 0x03u && equal(r.span(), bytes({0x04u, 0x10u, 0x05u, 0x10u, 0x06u})),
		      "03: byte count then big-endian registers");
	}
	{
		const Reply r = ask(model, kBoardUnit, 0x04u, bytes({0x00u, 0xFFu, 0x00u, 0x01u}));
		check(r.respond && equal(r.span(), bytes({0x02u, 0x20u, 0xFFu})), "04: the last input register");
	}
	{
		const Reply r = ask(model, kBoardUnit, 0x01u, bytes({0x00u, 0x00u, 0x00u, 0x0Au}));
		// coils 0..9: on at 0,3,6,9 -> bits 0,3,6 in byte 0 (0x49), bit 1 in byte 1 (0x02)
		check(r.respond && equal(r.span(), bytes({0x02u, 0x49u, 0x02u})), "01: bits packed LSB first, two bytes for ten coils");
	}
	{
		const Reply r = ask(model, kBoardUnit, 0x02u, bytes({0x00u, 0x08u, 0x00u, 0x08u}));
		check(r.respond && equal(r.span(), bytes({0x01u, 0x55u})), "02: eight discrete inputs from 8, alternating");
	}
	{
		std::vector<uint8_t> request{0x00u, 0x00u, 0x00u, 125u};
		const Reply r = ask(model, kBoardUnit, 0x03u, request);
		check(r.respond && r.size == 251u && r.data[0] == 250u, "03: 125 registers fill the largest legal response");
	}

	group("Writes");
	{
		const Reply r = ask(model, kBoardUnit, 0x06u, bytes({0x00u, 0x05u, 0xBEu, 0xEFu}));
		check(r.respond && r.function == 0x06u && equal(r.span(), bytes({0x00u, 0x05u, 0xBEu, 0xEFu})) &&
		      model.holding[5] == 0xBEEFu, "06: echoes the request and writes the register");
	}
	{
		const Reply r = ask(model, kBoardUnit, 0x05u, bytes({0x00u, 0x07u, 0xFFu, 0x00u}));
		check(r.respond && Model::bit(model.coils, 7), "05: 0xFF00 switches a coil on");
		const Reply off = ask(model, kBoardUnit, 0x05u, bytes({0x00u, 0x00u, 0x00u, 0x00u}));
		check(off.respond && !Model::bit(model.coils, 0), "05: 0x0000 switches it off");
	}
	{
		const Reply r = ask(model, kBoardUnit, 0x10u,
			bytes({0x00u, 0x0Au, 0x00u, 0x02u, 0x04u, 0xA0u, 0x00u, 0xA0u, 0x01u}));
		check(r.respond && equal(r.span(), bytes({0x00u, 0x0Au, 0x00u, 0x02u})) &&
		      model.holding[10] == 0xA000u && model.holding[11] == 0xA001u,
		      "10: answers start and quantity, writes the registers");
	}
	{
		const Reply r = ask(model, kBoardUnit, 0x0Fu, bytes({0x00u, 0x10u, 0x00u, 0x0Au, 0x02u, 0xCDu, 0x01u}));
		check(r.respond && equal(r.span(), bytes({0x00u, 0x10u, 0x00u, 0x0Au})) &&
		      Model::bit(model.coils, 16) && !Model::bit(model.coils, 17) && Model::bit(model.coils, 18) &&
		      Model::bit(model.coils, 24) && !Model::bit(model.coils, 25),
		      "0F: answers start and quantity, writes the coils LSB first");
	}
	{
		const Reply r = ask(model, kBoardUnit, 0x17u,
			bytes({0x00u, 0x00u, 0x00u, 0x02u, 0x00u, 0x64u, 0x00u, 0x01u, 0x02u, 0x12u, 0x34u}));
		check(r.respond && equal(r.span(), bytes({0x04u, 0x10u, 0x00u, 0x10u, 0x01u})) && model.holding[100] == 0x1234u,
		      "17: writes first, then reads");
	}
	{
		const Reply r = ask(model, kBoardUnit, 0x08u, bytes({0x00u, 0x00u, 0xA5u, 0x37u}));
		check(r.respond && r.function == 0x08u && equal(r.span(), bytes({0x00u, 0x00u, 0xA5u, 0x37u})),
		      "08/0000: returns the query data");
	}

	group("Exceptions");
	check(ask(model, kBoardUnit, 0x03u, bytes({0x00u, 0x00u, 0x00u, 0x00u})).code() == Exception::IllegalDataValue,
	      "03 with quantity 0: illegal data value");
	check(ask(model, kBoardUnit, 0x03u, bytes({0x00u, 0x00u, 0x00u, 126u})).code() == Exception::IllegalDataValue,
	      "03 with quantity 126: illegal data value");
	check(ask(model, kBoardUnit, 0x03u, bytes({0x00u, 0xFEu, 0x00u, 0x05u})).code() == Exception::IllegalDataAddress,
	      "03 past the end of the table: illegal data address");
	check(ask(model, kBoardUnit, 0x05u, bytes({0x00u, 0x01u, 0x12u, 0x34u})).code() == Exception::IllegalDataValue,
	      "05 with a value other than 0x0000/0xFF00: illegal data value");
	check(ask(model, kBoardUnit, 0x06u, bytes({0x01u, 0x00u, 0x00u, 0x01u})).code() == Exception::IllegalDataAddress,
	      "06 at address 256: illegal data address");
	check(ask(model, kBoardUnit, 0x10u, bytes({0x00u, 0x00u, 0x00u, 0x02u, 0x03u, 0x00u, 0x00u, 0x00u})).code() ==
	      Exception::IllegalDataValue, "10 with a byte count that disagrees with the quantity: illegal data value");
	check(ask(model, kBoardUnit, 0x64u, bytes({})).code() == Exception::IllegalFunction, "an unknown function: illegal function");
	check(ask(model, kBoardUnit, 0x08u, bytes({0x00u, 0x01u, 0x00u, 0x00u})).code() == Exception::IllegalFunction,
	      "08 with another sub-function: illegal function");
	{
		const Reply r = ask(model, kBoardUnit, 0x64u, bytes({}));
		check(r.respond && r.function == 0xE4u && r.size == 1u, "an exception reply is function | 0x80 and one code byte");
	}

	group("Script");
	{
		script::Request request;
		script::build(0u, kBoardUnit, request);
		check(request.address == kBoardUnit && request.function == 0x03u &&
		      equal(request.span(), bytes({0x00u, 0x00u, 0x00u, 0x0Au})), "step 0 reads holding 0..9 from the unit asked for");
		script::build(15u, kPcUnit, request);
		check(request.address == kPcUnit && request.function == 0x10u && request.size == 251u && request.data[4] == 246u,
		      "step 15 writes 123 registers: the largest legal request");
		script::build(script::kForeignUnitStep, kBoardUnit, request);
		check(request.address == kForeignUnit, "the foreign-unit step overrides the unit");
		script::build(script::kBroadcastStep, kBoardUnit, request);
		check(request.address == 0u && request.function == 0x06u, "the broadcast step addresses unit 0");
		Model shadow;
		shadow.reset();
		unsigned answered = 0u, silent = 0u, exceptions = 0u;
		for (std::size_t i = 0u; i < script::kSteps; ++i) {
			script::build(i, kBoardUnit, request);
			const Reply reply = serve(shadow, kBoardUnit, request.address, request.function, request.span());
			answered += reply.respond;
			silent += !reply.respond;
			exceptions += reply.respond && reply.exception();
		}
		check(answered == script::kSteps - 2u && silent == 2u && exceptions == 4u,
		      "against the reference model the script draws 53 answers, 2 silences and 4 exceptions");
		// step 12 wrote 100..103, then step 15 overwrote 20..142: the later write wins
		check(shadow.holding[5] == 0xBEEFu && shadow.holding[6] == 0x1234u && shadow.holding[100] == 0x7050u &&
		      shadow.holding[142] == 0x707Au && shadow.holding[19] == 0xA009u && Model::bit(shadow.coils, 7),
		      "and leaves every write, the broadcast included, in the model");
	}

	group("AddressingAndBroadcast");
	check(!ask(model, kForeignUnit, 0x03u, bytes({0x00u, 0x00u, 0x00u, 0x01u})).respond,
	      "a request for another unit is not answered");
	{
		const uint16_t before = model.holding[6];
		const Reply r = ask(model, 0x00u, 0x06u, bytes({0x00u, 0x06u, 0x12u, 0x34u}));
		check(!r.respond && model.holding[6] == 0x1234u && before != 0x1234u,
		      "a broadcast write executes and is not answered");
	}
	check(!ask(model, 0x00u, 0x03u, bytes({0x00u, 0x00u, 0x00u, 0x01u})).respond, "a broadcast read is not answered");
	{
		Model untouched;
		untouched.reset();
		(void)ask(untouched, kForeignUnit, 0x06u, bytes({0x00u, 0x00u, 0xFFu, 0xFFu}));
		check(untouched.holding[0] == 0x1000u, "a write for another unit leaves the model alone");
	}

	return finish();
}
