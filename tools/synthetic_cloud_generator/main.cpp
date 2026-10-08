#include "generator.h"

#include <QCoreApplication>
#include <QStringList>

#include <iostream>
#include <map>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        if (args.size() == 2 && args[1] == "--help") {
            std::cout << "Usage: synthetic-cloud-generator --geometry <file> --scan <file> "
                         "--output <file.xyz|file.asc>\n"
                         "Writes output and <output>.manifest.json; existing targets are rejected.\n";
            return 0;
        }
        if (args.size() == 2 && args[1] == "--version") {
            std::cout << synthetic::generatorVersion << '\n';
            return 0;
        }
        std::map<QString, QString> options;
        for (qsizetype i = 1; i < args.size(); i += 2) {
            if ((args[i] != "--geometry" && args[i] != "--scan" && args[i] != "--output")
                || i + 1 >= args.size() || args[i + 1].startsWith("--")
                || !options.emplace(args[i], args[i + 1]).second)
                throw synthetic::Error("invalid CLI arguments; use --help", 2);
        }
        if (options.size() != 3)
            throw synthetic::Error("--geometry, --scan and --output are required; use --help", 2);
        const auto result = synthetic::writeDataset(options.at("--geometry"), options.at("--scan"),
                                                    options.at("--output"));
        std::cout << "Generated " << result.pointCount << " points: "
                  << options.at("--output").toStdString() << " + manifest\n";
        return 0;
    } catch (const synthetic::Error &e) {
        std::cerr << e.what() << '\n';
        return e.exitCode;
    } catch (const std::exception &e) {
        std::cerr << "generation failed: " << e.what() << '\n';
        return 5;
    }
}
