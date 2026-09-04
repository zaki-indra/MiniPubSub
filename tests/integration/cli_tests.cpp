#include "minipubsub/minipubsub.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

extern char** environ;

namespace
{
struct ProcessResult {
    int exit_code;
    std::string output;
};

void write_all(int descriptor, std::string_view input) {
    while (!input.empty()) {
        const auto count = ::write(descriptor, input.data(), input.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        assert(count > 0);
        input.remove_prefix(static_cast<std::size_t>(count));
    }
}

ProcessResult run(const std::string& executable,
                  const std::vector<std::string>& arguments,
                  std::string_view input) {
    std::array<int, 2> child_input{};
    std::array<int, 2> child_output{};
    assert(::pipe(child_input.data()) == 0);
    assert(::pipe(child_output.data()) == 0);

    posix_spawn_file_actions_t actions;
    assert(::posix_spawn_file_actions_init(&actions) == 0);
    assert(::posix_spawn_file_actions_adddup2(&actions, child_input[0],
                                              STDIN_FILENO) == 0);
    assert(::posix_spawn_file_actions_adddup2(&actions, child_output[1],
                                              STDOUT_FILENO) == 0);
    assert(::posix_spawn_file_actions_adddup2(&actions, child_output[1],
                                              STDERR_FILENO) == 0);
    assert(::posix_spawn_file_actions_addclose(&actions, child_input[1]) == 0);
    assert(::posix_spawn_file_actions_addclose(&actions, child_output[0]) == 0);

    std::vector<std::string> owned_arguments;
    owned_arguments.push_back(executable);
    owned_arguments.insert(owned_arguments.end(), arguments.begin(), arguments.end());
    std::vector<char*> process_arguments;
    for (auto& argument : owned_arguments) {
        process_arguments.push_back(argument.data());
    }
    process_arguments.push_back(nullptr);

    pid_t process = 0;
    assert(::posix_spawn(&process, executable.c_str(), &actions, nullptr,
                         process_arguments.data(), environ) == 0);
    assert(::posix_spawn_file_actions_destroy(&actions) == 0);
    ::close(child_input[0]);
    ::close(child_output[1]);
    write_all(child_input[1], input);
    ::close(child_input[1]);

    std::string output;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(child_output[0], buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        assert(count >= 0);
        if (count == 0) {
            break;
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(child_output[0]);
    int status = 0;
    assert(::waitpid(process, &status, 0) == process);
    assert(WIFEXITED(status));
    return {WEXITSTATUS(status), std::move(output)};
}

void require_in_order(std::string_view output,
                      const std::vector<std::string_view>& expected) {
    std::size_t offset = 0;
    for (const auto text : expected) {
        const auto found = output.find(text, offset);
        assert(found != std::string_view::npos);
        offset = found + text.size();
    }
}
} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    mps_server_config_t* config = nullptr;
    mps_server_t* server = nullptr;
    assert(mps_server_config_create(&config, nullptr) == MPS_STATUS_OK);
    assert(mps_server_config_set_port(config, 0U, nullptr) == MPS_STATUS_OK);
    assert(mps_server_create(config, &server, nullptr) == MPS_STATUS_OK);
    mps_server_config_destroy(config);
    assert(mps_server_start(server, nullptr) == MPS_STATUS_OK);
    std::uint16_t port = 0;
    assert(mps_server_bound_port(server, &port, nullptr) == MPS_STATUS_OK);
    const std::string port_text = std::to_string(port);

    const auto interactive = run(
        argv[1], {"--host", "127.0.0.1", "--port", port_text},
        "PING\n"
        "SUBSCRIBE c1\n"
        "PUBLISH c1 \"hello world\"\n"
        "PUBLISH c3\n"
        "UNSUBSCRIBE c1\n"
        "HELP\n"
        "EXIT\n");
    assert(interactive.exit_code == 0);
    require_in_order(interactive.output,
                     {"> PONG", "> OK", "!msg \"c1\": hello world", "OK",
                      "ERROR: Invalid Argument", "OK", "Commands:", "bye. :)"});

    const auto one_shot = run(
        argv[1], {"--host", "127.0.0.1", "--port", port_text, "ping"}, "");
    assert(one_shot.exit_code == 0);
    assert(one_shot.output == "PONG\n");

    assert(mps_server_request_stop(server, nullptr) == MPS_STATUS_OK);
    assert(mps_server_wait(server, nullptr) == MPS_STATUS_OK);
    mps_server_destroy(server);
    return 0;
}
