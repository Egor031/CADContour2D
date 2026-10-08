#include <Eigen/Core>
#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <sstream>
#include <string>

TEST(DependencySmoke, LibrariesWorkTogether)
{
    const cv::Mat image = (cv::Mat_<double>(2, 2) << 1.0, 2.0, 3.0, 4.0);
    const double sum = cv::sum(image)[0];
    EXPECT_DOUBLE_EQ(sum, 10.0);

    Eigen::Matrix2d matrix;
    matrix << 1.0, 2.0, 3.0, 4.0;
    const Eigen::Vector2d result = matrix * Eigen::Vector2d(1.0, 2.0);
    EXPECT_DOUBLE_EQ(result.x(), 5.0);
    EXPECT_DOUBLE_EQ(result.y(), 11.0);

    std::ostringstream output;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    spdlog::logger logger("dependency-smoke", sink);
    logger.set_pattern("%v");
    logger.info("sum={} vector=({}, {})", sum, result.x(), result.y());
    logger.flush();
    EXPECT_NE(output.str().find("sum=10 vector=(5, 11)"), std::string::npos);
}
