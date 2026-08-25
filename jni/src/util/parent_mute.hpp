#pragma once

namespace Leticia {

/**
 * @brief Freezes the parent process during execution.
 */
class parent_mute final {
public:
    parent_mute() = default;
    ~parent_mute();

    parent_mute(const parent_mute &) = delete;
    parent_mute &operator=(const parent_mute &) = delete;

    /**
     * @brief Freezes the parent process.
     */
    void freeze();

    /**
     * @brief Resumes the parent process.
     */
    void resume();
};

} // namespace Leticia
