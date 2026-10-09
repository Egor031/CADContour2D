#pragma once

#include <optional>

namespace cadcontour {

enum class CommandOutcome { Applied, NoChange, Rejected };
enum class CommandError { InvalidValue, NoProject, UnsafeState };
enum class ErrorDetail { CellMustBeFiniteAndPositive, RevisionExhausted,
                         IdentityExhausted, HistoryExhausted, ReentrantCommand };

struct CommandFailure {
    CommandError category;
    std::optional<ErrorDetail> detail;
    std::optional<double> attemptedValue;
};

struct CommandResult {
    CommandOutcome outcome;
    std::optional<CommandFailure> failure;

    static CommandResult applied() { return {CommandOutcome::Applied, std::nullopt}; }
    static CommandResult noChange() { return {CommandOutcome::NoChange, std::nullopt}; }
    static CommandResult rejected(CommandError category,
                                  std::optional<ErrorDetail> detail = std::nullopt,
                                  std::optional<double> value = std::nullopt)
    {
        return {CommandOutcome::Rejected, CommandFailure{category, detail, value}};
    }
};

} // namespace cadcontour
