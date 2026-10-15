#include "TestHarness.hpp"
#include <SoapySidekiq/SampleScale.hpp>

TEST_CASE("SDK sample widths produce the correct signed full scale")
{
    REQUIRE_EQ(soapy_sidekiq::sampleFullScale(12), std::uint32_t{2047});
    REQUIRE_EQ(soapy_sidekiq::sampleFullScale(14), std::uint32_t{8191});
    REQUIRE_EQ(soapy_sidekiq::sampleFullScale(16), std::uint32_t{32767});
}

TEST_CASE("invalid SDK widths cannot cause shifts or unrepresentable scales")
{
    for (const std::uint8_t bits : {std::uint8_t{0}, std::uint8_t{1},
                                  std::uint8_t{17}, std::uint8_t{255}})
        REQUIRE_THROWS_AS(soapy_sidekiq::sampleFullScale(bits), std::invalid_argument);
}

TEST_CASE("TX host scales distinguish right-justified and upper-aligned DAC samples")
{
    using soapy_sidekiq::TxSampleAlignment;
    using soapy_sidekiq::txSampleFullScale;
    REQUIRE_EQ(txSampleFullScale(12, TxSampleAlignment::right_justified), std::uint32_t{2047});
    REQUIRE_EQ(txSampleFullScale(14, TxSampleAlignment::upper_bits), std::uint32_t{32767});
    REQUIRE_EQ(txSampleFullScale(16, TxSampleAlignment::right_justified), std::uint32_t{32767});
    REQUIRE_EQ(txSampleFullScale(14, TxSampleAlignment::right_justified), std::uint32_t{8191});
    REQUIRE_THROWS_AS(txSampleFullScale(0, TxSampleAlignment::upper_bits), std::invalid_argument);
    REQUIRE_THROWS_AS(txSampleFullScale(17, TxSampleAlignment::upper_bits), std::invalid_argument);
}
