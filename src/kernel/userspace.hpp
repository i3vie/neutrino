#pragma once

namespace process {
struct Process;
}

namespace userspace {

[[noreturn]] void enter_process(process::Process& proc);

}  // namespace userspace
