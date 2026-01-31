#include "platform.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <sstream>

std::atomic<bool> running{true};
SharedMemory* shared_mem = nullptr;
Mutex* global_mutex = nullptr;
LeaderElection* leader_election = nullptr;
std::atomic<int64_t> local_counter{0};

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        running = false;
        std::cout << "\nПолучен сигнал завершения. Завершаем работу...\n";
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int signum) {
    running = false;
    std::cout << "\nПолучен сигнал завершения. Завершаем работу...\n";
}
#endif

void increment_thread() {
    while (running) {
        sleep_ms(300);
        
        global_mutex->lock();
        SharedCounter* sc = shared_mem->get();
        sc->value++;
        local_counter = sc->value;
        global_mutex->unlock();
    }
}

void log_thread() {
    while (running) {
        sleep_ms(1000);
        
        if (!leader_election->is_current_leader()) continue;
        
        global_mutex->lock();
        SharedCounter* sc = shared_mem->get();
        int64_t current_value = sc->value;
        global_mutex->unlock();
        
        Timestamp ts = get_current_timestamp();
        char buffer[256];
        snprintf(buffer, sizeof(buffer), 
                "[%s] PID=%lld COUNTER=%lld",
                format_timestamp(ts).c_str(),
                (long long)get_current_pid(),
                (long long)current_value);
        log_message(buffer);
    }
}

void spawn_children_thread() {
    while (running) {
        sleep_ms(3000);
        
        if (!leader_election->is_current_leader()) continue;
        
        bool should_spawn = true;
        
        global_mutex->lock();
        SharedCounter* sc = shared_mem->get();
        
        if (sc->child1_pid != 0 && is_process_alive(sc->child1_pid)) {
            Timestamp ts = get_current_timestamp();
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "[%s] PID=%lld WARNING: Child1 (PID=%lld) still running, skipping spawn",
                    format_timestamp(ts).c_str(),
                    (long long)get_current_pid(),
                    (long long)sc->child1_pid);
            log_message(buffer);
            should_spawn = false;
        }
        
        if (sc->child2_pid != 0 && is_process_alive(sc->child2_pid)) {
            Timestamp ts = get_current_timestamp();
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "[%s] PID=%lld WARNING: Child2 (PID=%lld) still running, skipping spawn",
                    format_timestamp(ts).c_str(),
                    (long long)get_current_pid(),
                    (long long)sc->child2_pid);
            log_message(buffer);
            should_spawn = false;
        }
        
        if (should_spawn) {
            platform_pid_t pid1 = start_child_process("--child1");
            if (pid1 != 0) {
                sc->child1_pid = pid1;
                sc->child1_start_time = time(nullptr);
            }
            
            platform_pid_t pid2 = start_child_process("--child2");
            if (pid2 != 0) {
                sc->child2_pid = pid2;
                sc->child2_start_time = time(nullptr);
            }
        }
        
        global_mutex->unlock();
    }
}

void child1_logic() {
    Timestamp start_ts = get_current_timestamp();
    char buffer[256];
    snprintf(buffer, sizeof(buffer), 
            "[%s] CHILD1 START PID=%lld",
            format_timestamp(start_ts).c_str(),
            (long long)get_current_pid());
    log_message(buffer);
    
    global_mutex->lock();
    SharedCounter* sc = shared_mem->get();
    sc->value += 10;
    local_counter = sc->value;
    global_mutex->unlock();
    
    Timestamp end_ts = get_current_timestamp();
    snprintf(buffer, sizeof(buffer), 
            "[%s] CHILD1 END PID=%lld COUNTER=%lld",
            format_timestamp(end_ts).c_str(),
            (long long)get_current_pid(),
            (long long)local_counter.load());
    log_message(buffer);
    
#ifdef _WIN32
    ExitProcess(0);
#else
    exit(0);
#endif
}

void child2_logic() {
    Timestamp start_ts = get_current_timestamp();
    char buffer[256];
    snprintf(buffer, sizeof(buffer), 
            "[%s] CHILD2 START PID=%lld",
            format_timestamp(start_ts).c_str(),
            (long long)get_current_pid());
    log_message(buffer);
    
    global_mutex->lock();
    SharedCounter* sc = shared_mem->get();
    sc->value *= 2;
    local_counter = sc->value;
    global_mutex->unlock();
    
    sleep_ms(2000);
    
    global_mutex->lock();
    sc = shared_mem->get();
    sc->value /= 2;
    local_counter = sc->value;
    global_mutex->unlock();
    
    Timestamp end_ts = get_current_timestamp();
    snprintf(buffer, sizeof(buffer), 
            "[%s] CHILD2 END PID=%lld COUNTER=%lld",
            format_timestamp(end_ts).c_str(),
            (long long)get_current_pid(),
            (long long)local_counter.load());
    log_message(buffer);
    
#ifdef _WIN32
    ExitProcess(0);
#else
    exit(0);
#endif
}

void command_loop() {
    std::cout << "\n=== СЧЕТЧИК ЗАПУЩЕН ===\n";
    std::cout << "PID процесса: " << get_current_pid() << "\n";
    std::cout << "Режим: " << (leader_election->is_current_leader() ? "ЛИДЕР" : "УЧАСТНИК") << "\n";
    std::cout << "Доступные команды:\n";
    std::cout << "  set N   - установить значение счетчика N\n";
    std::cout << "  get     - показать текущее значение\n";
    std::cout << "  exit    - завершить программу\n\n";
    
    while (running) {
        std::cout << "counter> ";
        std::string cmd;
        std::getline(std::cin, cmd);
        
        if (cmd.empty()) continue;
        
        if (cmd == "exit") {
            running = false;
            break;
        }
        
        if (cmd == "get") {
            std::cout << "Текущее значение счетчика: " << local_counter.load() << "\n";
            continue;
        }
        
        if (cmd.substr(0, 4) == "set ") {
            try {
                int64_t new_value = std::stoll(cmd.substr(4));
                
                global_mutex->lock();
                SharedCounter* sc = shared_mem->get();
                sc->value = new_value;
                local_counter = new_value;
                global_mutex->unlock();
                
                std::cout << "Счетчик установлен в " << new_value << "\n";
                
                Timestamp ts = get_current_timestamp();
                char buffer[256];
                snprintf(buffer, sizeof(buffer), 
                        "[%s] PID=%lld MANUAL_SET COUNTER=%lld",
                        format_timestamp(ts).c_str(),
                        (long long)get_current_pid(),
                        (long long)new_value);
                log_message(buffer);
            } catch (...) {
                std::cout << "Ошибка: неверный формат числа\n";
            }
            continue;
        }
        
        std::cout << "Неизвестная команда. Доступные: set N, get, exit\n";
    }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)console_handler, TRUE);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
#endif
    
    shared_mem = new SharedMemory();
    global_mutex = new Mutex();
    leader_election = new LeaderElection();
    
    if (argc > 1) {
        if (strcmp(argv[1], "--child1") == 0) {
            child1_logic();
            return 0;
        } else if (strcmp(argv[1], "--child2") == 0) {
            child2_logic();
            return 0;
        }
    }
    
    Timestamp start_ts = get_current_timestamp();
    char buffer[256];
    snprintf(buffer, sizeof(buffer), 
            "[%s] MAIN START PID=%lld (Leader: %s)",
            format_timestamp(start_ts).c_str(),
            (long long)get_current_pid(),
            leader_election->is_current_leader() ? "YES" : "NO");
    log_message(buffer);
    
    global_mutex->lock();
    SharedCounter* sc = shared_mem->get();
    local_counter = sc->value;
    global_mutex->unlock();
    
    std::thread inc_thread(increment_thread);
    std::thread log_thr;
    std::thread spawn_thr;
    
    if (leader_election->is_current_leader()) {
        log_thr = std::thread(log_thread);
        spawn_thr = std::thread(spawn_children_thread);
    }
    
    command_loop();
    
    running = false;
    if (inc_thread.joinable()) inc_thread.join();
    if (log_thr.joinable()) log_thr.join();
    if (spawn_thr.joinable()) spawn_thr.join();
    
    delete leader_election;
    delete global_mutex;
    delete shared_mem;
    
    Timestamp end_ts = get_current_timestamp();
    snprintf(buffer, sizeof(buffer), 
            "[%s] MAIN EXIT PID=%lld COUNTER=%lld",
            format_timestamp(end_ts).c_str(),
            (long long)get_current_pid(),
            (long long)local_counter.load());
    log_message(buffer);
    
    return 0;
}