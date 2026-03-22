#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "crosspad/led/RgbColor.hpp"
#include "crosspad/led/HslColor.hpp"

using namespace crosspad;

TEST_CASE("RgbColor: default constructor is black", "[color]") {
    RgbColor c;
    REQUIRE(c.R == 0);
    REQUIRE(c.G == 0);
    REQUIRE(c.B == 0);
}

TEST_CASE("RgbColor: parameterized constructor", "[color]") {
    RgbColor c(128, 64, 32);
    REQUIRE(c.R == 128);
    REQUIRE(c.G == 64);
    REQUIRE(c.B == 32);
}

TEST_CASE("RgbColor: equality", "[color]") {
    REQUIRE(RgbColor(255, 0, 0) == RgbColor(255, 0, 0));
    REQUIRE_FALSE(RgbColor(255, 0, 0) == RgbColor(0, 255, 0));
}

TEST_CASE("RgbColor: Dim", "[color]") {
    RgbColor c(200, 100, 50);

    SECTION("full brightness returns same color") {
        auto d = c.Dim(255);
        REQUIRE(d.R == 200);
        REQUIRE(d.G == 100);
        REQUIRE(d.B == 50);
    }

    SECTION("zero brightness returns black") {
        auto d = c.Dim(0);
        REQUIRE(d.R == 0);
        REQUIRE(d.G == 0);
        REQUIRE(d.B == 0);
    }

    SECTION("half brightness halves values") {
        auto d = c.Dim(128);
        // 200 * 128/255 ≈ 100, 100 * 128/255 ≈ 50, 50 * 128/255 ≈ 25
        REQUIRE(d.R >= 99);
        REQUIRE(d.R <= 101);
    }
}

TEST_CASE("RgbColor: LinearBlend", "[color]") {
    RgbColor black(0, 0, 0);
    RgbColor white(255, 255, 255);

    SECTION("t=0 returns start") {
        auto r = RgbColor::LinearBlend(black, white, 0.0f);
        REQUIRE(r == black);
    }

    SECTION("t=1 returns end") {
        auto r = RgbColor::LinearBlend(black, white, 1.0f);
        REQUIRE(r == white);
    }

    SECTION("t=0.5 returns midpoint") {
        auto r = RgbColor::LinearBlend(black, white, 0.5f);
        REQUIRE(r.R >= 126);
        REQUIRE(r.R <= 128);
    }

    SECTION("t is clamped") {
        auto r = RgbColor::LinearBlend(black, white, 2.0f);
        REQUIRE(r == white);
        auto r2 = RgbColor::LinearBlend(black, white, -1.0f);
        REQUIRE(r2 == black);
    }
}

TEST_CASE("RgbColor: HSL conversion", "[color]") {
    SECTION("red") {
        RgbColor c(HslColor(0.0f, 1.0f, 0.5f));
        REQUIRE(c.R == 255);
        REQUIRE(c.G == 0);
        REQUIRE(c.B == 0);
    }

    SECTION("green") {
        RgbColor c(HslColor(120.0f, 1.0f, 0.5f));
        REQUIRE(c.R == 0);
        REQUIRE(c.G == 255);
        REQUIRE(c.B == 0);
    }

    SECTION("blue") {
        RgbColor c(HslColor(240.0f, 1.0f, 0.5f));
        REQUIRE(c.R == 0);
        REQUIRE(c.G == 0);
        REQUIRE(c.B == 255);
    }

    SECTION("white") {
        RgbColor c(HslColor(0.0f, 0.0f, 1.0f));
        REQUIRE(c.R == 255);
        REQUIRE(c.G == 255);
        REQUIRE(c.B == 255);
    }

    SECTION("black") {
        RgbColor c(HslColor(0.0f, 0.0f, 0.0f));
        REQUIRE(c.R == 0);
        REQUIRE(c.G == 0);
        REQUIRE(c.B == 0);
    }
}
