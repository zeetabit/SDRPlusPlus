#include <catch.hpp>
#include <api_version.h>

TEST_CASE("API version constants", "[api_version]") {
    REQUIRE(SDRPP_API_VERSION_MAJOR == 2);
    REQUIRE(SDRPP_API_VERSION_MINOR == 0);
    REQUIRE(SDRPP_API_VERSION_PATCH == 0);

    REQUIRE(SDRPP_API_VERSION == SDRPP_MAKE_API_VERSION(2, 0, 0));
}

TEST_CASE("API version encoding", "[api_version]") {
    int v = SDRPP_MAKE_API_VERSION(3, 5, 12);
    REQUIRE(((v >> 16) & 0xFF) == 3);
    REQUIRE(((v >> 8) & 0xFF) == 5);
    REQUIRE((v & 0xFF) == 12);
}

TEST_CASE("API compatibility check", "[api_version]") {
    SECTION("same version is compatible") {
        REQUIRE(sdrppApiCompatible(SDRPP_API_VERSION));
    }

    SECTION("same major, lower minor is compatible") {
        REQUIRE(sdrppApiCompatible(SDRPP_MAKE_API_VERSION(2, 0, 0)));
    }

    SECTION("same major, higher minor is incompatible") {
        REQUIRE_FALSE(sdrppApiCompatible(SDRPP_MAKE_API_VERSION(2, 1, 0)));
    }

    SECTION("different major is incompatible") {
        REQUIRE_FALSE(sdrppApiCompatible(SDRPP_MAKE_API_VERSION(1, 0, 0)));
        REQUIRE_FALSE(sdrppApiCompatible(SDRPP_MAKE_API_VERSION(3, 0, 0)));
    }

    SECTION("patch version does not affect compatibility") {
        REQUIRE(sdrppApiCompatible(SDRPP_MAKE_API_VERSION(2, 0, 99)));
    }
}
