/*
	test_script_text — the bounded history of drawn text behind `wait text ... recent`
*/

#include <doctest/doctest.h>
#include "debugger/text_history.h"
#include "debugger/cmd_parser.h"

TEST_CASE("text history: find returns the newest match not older than 'since'")
{
	TextHistory h;
	h.push(100, "Open", "DrawString");
	h.push(200, "Cannot open MacTCP driver", "DrawText");
	h.push(300, "OK", "DrawString");

	auto *r = h.find("MacTCP", 0);
	REQUIRE(r != nullptr);
	CHECK(r->time == 200);
	CHECK(r->trap == "DrawText");

	CHECK(h.find("MacTCP", 200) != nullptr); // drawn exactly at 'since' counts
	CHECK(h.find("MacTCP", 201) == nullptr); // before 'since': invisible
	CHECK(h.find("Special", 0) == nullptr);

	h.push(400, "Open again", "DrawString");
	r = h.find("Open", 0);
	REQUIRE(r != nullptr);
	CHECK(r->time == 400); // the newest of several matches
}

TEST_CASE("text history: bounded, oldest dropped first")
{
	TextHistory h(3);
	for (uint64_t i = 1; i <= 5; ++i) h.push(i, "s" + std::to_string(i), "t");
	CHECK(h.size() == 3);
	CHECK(h.find("s1", 0) == nullptr);
	CHECK(h.find("s2", 0) == nullptr);
	CHECK(h.find("s3", 0) != nullptr);
	auto l = h.last(10);
	REQUIRE(l.size() == 3);
	CHECK(l[0]->text == "s3"); // oldest first
	CHECK(l[2]->text == "s5");
}

TEST_CASE("text history: last(n) and clear")
{
	TextHistory h;
	CHECK(h.last(5).empty());
	h.push(1, "a", "t");
	h.push(2, "b", "t");
	h.push(3, "c", "t");
	auto l = h.last(2);
	REQUIRE(l.size() == 2);
	CHECK(l[0]->text == "b");
	CHECK(l[1]->text == "c");
	h.clear();
	CHECK(h.size() == 0);
	CHECK(h.find("a", 0) == nullptr);
}

TEST_CASE("text history: zero capacity is treated as one")
{
	TextHistory h(0);
	h.push(1, "a", "t");
	h.push(2, "b", "t");
	CHECK(h.size() == 1);
	CHECK(h.find("b", 0) != nullptr);
}

TEST_CASE("text history: failure listing")
{
	TextHistory h;
	CHECK(FormatTextHistory(h, 5, 1000) == "  recent text: none drawn\n");
	h.push(2500, "Special", "DrawString");
	h.push(3000, "say \"hi\"", "DrawText");
	h.push(4000, "OK", "DrawString");
	auto s = FormatTextHistory(h, 2, 1000);
	CHECK(s.find("2 of 3") != std::string::npos);
	CHECK(s.find("Special") == std::string::npos); // only the newest two
	CHECK(s.find("3.000s] DrawText \"say \"hi\"\"") != std::string::npos);
	CHECK(s.find("4.000s] DrawString \"OK\"") != std::string::npos);
	CHECK(s.find("DrawText") < s.find("\"OK\"")); // oldest first
}

TEST_CASE("tokenizer: quoted words are marked, so `log \"file\"` is a message")
{
	auto q = Tokenize("log \"file\" 3");
	REQUIRE(q.size() >= 3);
	CHECK(q[1].isWord("file"));
	CHECK(q[1].quoted);
	CHECK_FALSE(q[0].quoted);
	auto u = Tokenize("log file");
	REQUIRE(u.size() >= 2);
	CHECK_FALSE(u[1].quoted);
}
