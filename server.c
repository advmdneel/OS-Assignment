/*
 * Concurrent Networked Board Game Server
 * Race to 100 - A multiplayer dice racing game
 * 
 * Hybrid Concurrency Model:
 * - fork() for client handlers (one child process per player)
 * - pthreads for scheduler and logger threads
 * - POSIX shared memory for game state
 * - Named pipes for client-server IPC
 */

#include "common.h"

// Global pointers to shared memory
SharedGameState *game_state = NULL;
LogQueue *log_queue = NULL;
SharedScores *scores = NULL;

// Thread handles
pthread_t scheduler_thread;
pthread_t logger_thread;

// Server running flag
volatile sig_atomic_t server_running = 1;

// File descriptors for shared memory
int shm_game_fd = -1;
int shm_log_fd = -1;
int shm_scores_fd = -1;

// ============================================================================
// Utility Functions
// ============================================================================

void init_process_shared_mutex(pthread_mutex_t *mutex) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void init_process_shared_cond(pthread_cond_t *cond) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

// ============================================================================
// Logging Functions
// ============================================================================

void enqueue_log(const char *format, ...) {
    if (!log_queue) return;
    
    char message[MAX_LOG_MSG];
    va_list args;
    va_start(args, format);
    vsnprintf(message, MAX_LOG_MSG, format, args);
    va_end(args);
    
    pthread_mutex_lock(&log_queue->mutex);
    
    if (log_queue->count < LOG_QUEUE_SIZE) {
        LogEntry *entry = &log_queue->entries[log_queue->tail];
        strncpy(entry->message, message, MAX_LOG_MSG - 1);
        entry->message[MAX_LOG_MSG - 1] = '\0';
        entry->timestamp = time(NULL);
        
        log_queue->tail = (log_queue->tail + 1) % LOG_QUEUE_SIZE;
        log_queue->count++;
        
        pthread_cond_signal(&log_queue->not_empty);
    }
    
    pthread_mutex_unlock(&log_queue->mutex);
}

// Logger Thread Function
void *logger_thread_func(void *arg) {
    (void)arg;
    FILE *log_file = fopen(LOG_FILE, "a");
    if (!log_file) {
        perror("Failed to open log file");
        return NULL;
    }
    
    // Set line buffering for real-time logging
    setvbuf(log_file, NULL, _IOLBF, 0);
    
    printf("[Logger] Logger thread started\n");
    
    while (1) {
        pthread_mutex_lock(&log_queue->mutex);
        
        // Wait for log entries
        while (log_queue->count == 0 && !log_queue->shutdown) {
            pthread_cond_wait(&log_queue->not_empty, &log_queue->mutex);
        }
        
        // Process all available log entries
        while (log_queue->count > 0) {
            LogEntry *entry = &log_queue->entries[log_queue->head];
            
            // Format timestamp
            struct tm *tm_info = localtime(&entry->timestamp);
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            
            // Write to log file (atomic write)
            fprintf(log_file, "[%s] %s\n", time_str, entry->message);
            fflush(log_file);
            
            log_queue->head = (log_queue->head + 1) % LOG_QUEUE_SIZE;
            log_queue->count--;
        }
        
        int should_exit = log_queue->shutdown && log_queue->count == 0;
        pthread_mutex_unlock(&log_queue->mutex);
        
        if (should_exit) break;
    }
    
    fclose(log_file);
    printf("[Logger] Logger thread terminated\n");
    return NULL;
}

// ============================================================================
// Score Persistence Functions
// ============================================================================

void load_scores(void) {
    FILE *file = fopen(SCORES_FILE, "r");
    if (!file) {
        printf("[Server] No existing scores file, starting fresh\n");
        return;
    }
    
    pthread_mutex_lock(&scores->mutex);
    
    char name[MAX_NAME_LEN];
    int wins;
    while (fscanf(file, "%31s %d", name, &wins) == 2 && scores->count < MAX_SCORES) {
        strncpy(scores->entries[scores->count].name, name, MAX_NAME_LEN - 1);
        scores->entries[scores->count].wins = wins;
        scores->count++;
    }
    
    pthread_mutex_unlock(&scores->mutex);
    fclose(file);
    
    enqueue_log("Loaded %d score entries from %s", scores->count, SCORES_FILE);
}

void save_scores(void) {
    FILE *file = fopen(SCORES_FILE, "w");
    if (!file) {
        perror("Failed to save scores");
        return;
    }
    
    pthread_mutex_lock(&scores->mutex);
    
    for (int i = 0; i < scores->count; i++) {
        fprintf(file, "%s %d\n", scores->entries[i].name, scores->entries[i].wins);
    }
    
    pthread_mutex_unlock(&scores->mutex);
    fclose(file);
    
    enqueue_log("Saved %d score entries to %s", scores->count, SCORES_FILE);
}

void update_score(const char *player_name) {
    pthread_mutex_lock(&scores->mutex);
    
    // Find existing entry or create new
    int found = -1;
    for (int i = 0; i < scores->count; i++) {
        if (strcmp(scores->entries[i].name, player_name) == 0) {
            found = i;
            break;
        }
    }
    
    if (found >= 0) {
        scores->entries[found].wins++;
    } else if (scores->count < MAX_SCORES) {
        strncpy(scores->entries[scores->count].name, player_name, MAX_NAME_LEN - 1);
        scores->entries[scores->count].wins = 1;
        scores->count++;
    }
    
    pthread_mutex_unlock(&scores->mutex);
    
    // Save to file after update
    save_scores();
}

// ============================================================================
// Round Robin Scheduler Thread
// ============================================================================

int get_next_active_player(int current) {
    int start = (current + 1) % MAX_PLAYERS;
    int idx = start;
    
    do {
        if (game_state->players[idx].state == PLAYER_ACTIVE) {
            return idx;
        }
        idx = (idx + 1) % MAX_PLAYERS;
    } while (idx != start);
    
    return -1; // No active players
}

int count_active_players(void) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game_state->players[i].state == PLAYER_ACTIVE) {
            count++;
        }
    }
    return count;
}

void *scheduler_thread_func(void *arg) {
    (void)arg;
    printf("[Scheduler] Scheduler thread started\n");
    enqueue_log("Round Robin Scheduler initialized");
    
    while (server_running) {
        pthread_mutex_lock(&game_state->game_mutex);
        
        if (game_state->game_state == GAME_IN_PROGRESS) {
            // Check if current player is still active
            int current = game_state->current_turn;
            
            if (current >= 0 && current < MAX_PLAYERS) {
                if (game_state->players[current].state != PLAYER_ACTIVE) {
                    // Current player disconnected, find next
                    int next = get_next_active_player(current);
                    if (next >= 0) {
                        game_state->current_turn = next;
                        enqueue_log("Scheduler: Skipped inactive player, turn passed to Player %d (%s)",
                                   next + 1, game_state->players[next].name);
                        pthread_cond_broadcast(&game_state->turn_cond);
                    } else {
                        // No active players, end game
                        game_state->game_state = GAME_FINISHED;
                        enqueue_log("Scheduler: No active players, game ended");
                    }
                }
            }
            
            // Check for winner
            if (game_state->winner_id >= 0) {
                game_state->game_state = GAME_FINISHED;
                pthread_cond_broadcast(&game_state->turn_cond);
            }
        }
        
        pthread_mutex_unlock(&game_state->game_mutex);
        
        // Small sleep to prevent busy waiting
        usleep(50000); // 50ms
    }
    
    printf("[Scheduler] Scheduler thread terminated\n");
    return NULL;
}

void advance_turn(void) {
    pthread_mutex_lock(&game_state->game_mutex);
    
    int next = get_next_active_player(game_state->current_turn);
    if (next >= 0) {
        game_state->current_turn = next;
        enqueue_log("Turn advanced to Player %d (%s)", 
                   next + 1, game_state->players[next].name);
    }
    
    pthread_cond_broadcast(&game_state->turn_cond);
    pthread_mutex_unlock(&game_state->game_mutex);
}

// ============================================================================
// Client Handler (Child Process)
// ============================================================================

void handle_client(int player_id, int pipe_read_fd, int pipe_write_fd) {
    Player *player = &game_state->players[player_id];
    GameMessage msg, response;
    
    printf("[Handler %d] Started for player %d\n", getpid(), player_id + 1);
    enqueue_log("Handler process started for Player %d", player_id + 1);
    
    // Seed random number generator uniquely for this process
    srand(time(NULL) ^ getpid());
    
    while (1) {
        // Read message from client
        ssize_t bytes = read(pipe_read_fd, &msg, sizeof(GameMessage));
        if (bytes <= 0) {
            // Client disconnected
            pthread_mutex_lock(&game_state->game_mutex);
            player->state = PLAYER_DISCONNECTED;
            pthread_mutex_unlock(&game_state->game_mutex);
            
            enqueue_log("Player %d (%s) disconnected", player_id + 1, player->name);
            break;
        }
        
        memset(&response, 0, sizeof(GameMessage));
        
        switch (msg.type) {
            case MSG_JOIN: {
                pthread_mutex_lock(&game_state->game_mutex);
                
                strncpy(player->name, msg.player_name, MAX_NAME_LEN - 1);
                player->state = PLAYER_WAITING;
                player->score = 0;
                game_state->num_players++;
                
                response.type = MSG_PLAYER_JOINED;
                response.player_id = player_id;
                snprintf(response.text, MAX_LOG_MSG, 
                        "Welcome %s! You are Player %d. Waiting for %d more players...",
                        player->name, player_id + 1, 
                        MIN_PLAYERS - game_state->num_players);
                
                enqueue_log("Player %d joined: %s (Total: %d players)", 
                           player_id + 1, player->name, game_state->num_players);
                
                // Check if we can start the game
                if (game_state->num_players >= MIN_PLAYERS && 
                    game_state->game_state == GAME_WAITING_FOR_PLAYERS) {
                    game_state->game_state = GAME_IN_PROGRESS;
                    game_state->current_turn = 0;
                    
                    // Find first active player
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        if (game_state->players[i].state == PLAYER_WAITING) {
                            game_state->players[i].state = PLAYER_ACTIVE;
                        }
                    }
                    game_state->current_turn = get_next_active_player(-1);
                    
                    enqueue_log("Game started with %d players! First turn: Player %d",
                               game_state->num_players, game_state->current_turn + 1);
                }
                
                pthread_cond_broadcast(&game_state->turn_cond);
                pthread_mutex_unlock(&game_state->game_mutex);
                
                write(pipe_write_fd, &response, sizeof(GameMessage));
                break;
            }
            
            case MSG_ROLL: {
                pthread_mutex_lock(&game_state->game_mutex);
                
                // Check if it's this player's turn
                if (game_state->game_state != GAME_IN_PROGRESS) {
                    response.type = MSG_ERROR;
                    snprintf(response.text, MAX_LOG_MSG, "Game not in progress");
                    pthread_mutex_unlock(&game_state->game_mutex);
                    write(pipe_write_fd, &response, sizeof(GameMessage));
                    break;
                }
                
                if (game_state->current_turn != player_id) {
                    response.type = MSG_WAIT;
                    snprintf(response.text, MAX_LOG_MSG, 
                            "Not your turn! Current turn: Player %d (%s)",
                            game_state->current_turn + 1,
                            game_state->players[game_state->current_turn].name);
                    pthread_mutex_unlock(&game_state->game_mutex);
                    write(pipe_write_fd, &response, sizeof(GameMessage));
                    break;
                }
                
                // Roll two dice (server-side randomness)
                int die1 = (rand() % 6) + 1;
                int die2 = (rand() % 6) + 1;
                int roll_total = die1 + die2;
                
                // Special rule: Rolling double 1s resets score to 0
                if (die1 == 1 && die2 == 1) {
                    player->score = 0;
                    response.type = MSG_ROLL_RESULT;
                    response.value = 0;
                    snprintf(response.text, MAX_LOG_MSG,
                            "Snake eyes! You rolled %d + %d. Score reset to 0!",
                            die1, die2);
                    enqueue_log("Player %d (%s) rolled snake eyes! Score reset to 0",
                               player_id + 1, player->name);
                } else {
                    player->score += roll_total;
                    response.type = MSG_ROLL_RESULT;
                    response.value = player->score;
                    snprintf(response.text, MAX_LOG_MSG,
                            "You rolled %d + %d = %d. Total score: %d",
                            die1, die2, roll_total, player->score);
                    enqueue_log("Player %d (%s) rolled %d + %d = %d (Total: %d)",
                               player_id + 1, player->name, die1, die2, roll_total, player->score);
                }
                
                // Check for winner
                if (player->score >= WINNING_SCORE) {
                    game_state->winner_id = player_id;
                    game_state->game_state = GAME_FINISHED;
                    response.type = MSG_GAME_OVER;
                    snprintf(response.text, MAX_LOG_MSG,
                            "CONGRATULATIONS! You reached %d and WON!",
                            player->score);
                    enqueue_log("GAME OVER: Player %d (%s) wins with %d points!",
                               player_id + 1, player->name, player->score);
                    
                    // Update persistent scores
                    update_score(player->name);
                    
                    pthread_cond_broadcast(&game_state->turn_cond);
                    pthread_mutex_unlock(&game_state->game_mutex);
                    write(pipe_write_fd, &response, sizeof(GameMessage));
                    break;
                }
                
                pthread_mutex_unlock(&game_state->game_mutex);
                
                // Advance turn
                advance_turn();
                
                write(pipe_write_fd, &response, sizeof(GameMessage));
                break;
            }
            
            case MSG_GAME_STATE: {
                pthread_mutex_lock(&game_state->game_mutex);
                
                response.type = MSG_GAME_STATE;
                char *ptr = response.text;
                int remaining = MAX_LOG_MSG;
                
                ptr += snprintf(ptr, remaining, "\n=== GAME STATE ===\n");
                remaining = MAX_LOG_MSG - (ptr - response.text);
                
                ptr += snprintf(ptr, remaining, "Status: %s\n",
                        game_state->game_state == GAME_WAITING_FOR_PLAYERS ? "Waiting for players" :
                        game_state->game_state == GAME_IN_PROGRESS ? "In progress" : "Finished");
                remaining = MAX_LOG_MSG - (ptr - response.text);
                
                for (int i = 0; i < MAX_PLAYERS && remaining > 50; i++) {
                    if (game_state->players[i].state != PLAYER_DISCONNECTED) {
                        ptr += snprintf(ptr, remaining, "Player %d: %s - Score: %d %s\n",
                                i + 1,
                                game_state->players[i].name,
                                game_state->players[i].score,
                                (game_state->current_turn == i) ? "<-- TURN" : "");
                        remaining = MAX_LOG_MSG - (ptr - response.text);
                    }
                }
                
                pthread_mutex_unlock(&game_state->game_mutex);
                write(pipe_write_fd, &response, sizeof(GameMessage));
                break;
            }
            
            case MSG_QUIT: {
                pthread_mutex_lock(&game_state->game_mutex);
                player->state = PLAYER_DISCONNECTED;
                game_state->num_players--;
                pthread_cond_broadcast(&game_state->turn_cond);
                pthread_mutex_unlock(&game_state->game_mutex);
                
                enqueue_log("Player %d (%s) quit the game", player_id + 1, player->name);
                
                response.type = MSG_PLAYER_LEFT;
                snprintf(response.text, MAX_LOG_MSG, "Goodbye %s!", player->name);
                write(pipe_write_fd, &response, sizeof(GameMessage));
                goto cleanup;
            }
            
            default:
                response.type = MSG_ERROR;
                snprintf(response.text, MAX_LOG_MSG, "Unknown command");
                write(pipe_write_fd, &response, sizeof(GameMessage));
                break;
        }
    }
    
cleanup:
    close(pipe_read_fd);
    close(pipe_write_fd);
    printf("[Handler %d] Exiting\n", getpid());
    exit(0);
}

// ============================================================================
// Signal Handlers
// ============================================================================

void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    
    // Reap all zombie child processes
    while (waitpid(-1, NULL, WNOHANG) > 0);
    
    errno = saved_errno;
}

void sigint_handler(int sig) {
    (void)sig;
    printf("\n[Server] Shutdown signal received...\n");
    server_running = 0;
    
    // Save scores before shutdown
    save_scores();
    
    // Signal logger to shutdown
    if (log_queue) {
        pthread_mutex_lock(&log_queue->mutex);
        log_queue->shutdown = 1;
        pthread_cond_signal(&log_queue->not_empty);
        pthread_mutex_unlock(&log_queue->mutex);
    }
}

// ============================================================================
// Shared Memory Setup
// ============================================================================

int setup_shared_memory(void) {
    // Create shared memory for game state
    shm_game_fd = shm_open(SHM_GAME_STATE, O_CREAT | O_RDWR, 0666);
    if (shm_game_fd < 0) {
        perror("shm_open game state");
        return -1;
    }
    ftruncate(shm_game_fd, sizeof(SharedGameState));
    game_state = mmap(NULL, sizeof(SharedGameState), PROT_READ | PROT_WRITE, 
                      MAP_SHARED, shm_game_fd, 0);
    if (game_state == MAP_FAILED) {
        perror("mmap game state");
        return -1;
    }
    
    // Create shared memory for log queue
    shm_log_fd = shm_open(SHM_LOG_QUEUE, O_CREAT | O_RDWR, 0666);
    if (shm_log_fd < 0) {
        perror("shm_open log queue");
        return -1;
    }
    ftruncate(shm_log_fd, sizeof(LogQueue));
    log_queue = mmap(NULL, sizeof(LogQueue), PROT_READ | PROT_WRITE,
                     MAP_SHARED, shm_log_fd, 0);
    if (log_queue == MAP_FAILED) {
        perror("mmap log queue");
        return -1;
    }
    
    // Create shared memory for scores
    shm_scores_fd = shm_open(SHM_SCORES, O_CREAT | O_RDWR, 0666);
    if (shm_scores_fd < 0) {
        perror("shm_open scores");
        return -1;
    }
    ftruncate(shm_scores_fd, sizeof(SharedScores));
    scores = mmap(NULL, sizeof(SharedScores), PROT_READ | PROT_WRITE,
                  MAP_SHARED, shm_scores_fd, 0);
    if (scores == MAP_FAILED) {
        perror("mmap scores");
        return -1;
    }
    
    // Initialize game state
    memset(game_state, 0, sizeof(SharedGameState));
    game_state->game_state = GAME_WAITING_FOR_PLAYERS;
    game_state->current_turn = -1;
    game_state->winner_id = -1;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game_state->players[i].id = i;
        game_state->players[i].state = PLAYER_DISCONNECTED;
        game_state->players[i].score = 0;
    }
    
    // Initialize process-shared synchronization primitives
    init_process_shared_mutex(&game_state->game_mutex);
    init_process_shared_mutex(&game_state->turn_mutex);
    init_process_shared_cond(&game_state->turn_cond);
    
    // Initialize log queue
    memset(log_queue, 0, sizeof(LogQueue));
    init_process_shared_mutex(&log_queue->mutex);
    init_process_shared_cond(&log_queue->not_empty);
    
    // Initialize scores
    memset(scores, 0, sizeof(SharedScores));
    init_process_shared_mutex(&scores->mutex);
    
    return 0;
}

void cleanup_shared_memory(void) {
    // Destroy synchronization primitives
    if (game_state) {
        pthread_mutex_destroy(&game_state->game_mutex);
        pthread_mutex_destroy(&game_state->turn_mutex);
        pthread_cond_destroy(&game_state->turn_cond);
        munmap(game_state, sizeof(SharedGameState));
    }
    
    if (log_queue) {
        pthread_mutex_destroy(&log_queue->mutex);
        pthread_cond_destroy(&log_queue->not_empty);
        munmap(log_queue, sizeof(LogQueue));
    }
    
    if (scores) {
        pthread_mutex_destroy(&scores->mutex);
        munmap(scores, sizeof(SharedScores));
    }
    
    // Unlink shared memory
    shm_unlink(SHM_GAME_STATE);
    shm_unlink(SHM_LOG_QUEUE);
    shm_unlink(SHM_SCORES);
    
    // Remove named pipes
    for (int i = 0; i < MAX_PLAYERS; i++) {
        char pipe_name[64];
        snprintf(pipe_name, sizeof(pipe_name), "%s%d_to_server", PIPE_BASE, i);
        unlink(pipe_name);
        snprintf(pipe_name, sizeof(pipe_name), "%s%d_to_client", PIPE_BASE, i);
        unlink(pipe_name);
    }
}

void reset_game_state(void) {
    pthread_mutex_lock(&game_state->game_mutex);
    
    game_state->game_state = GAME_WAITING_FOR_PLAYERS;
    game_state->current_turn = -1;
    game_state->winner_id = -1;
    game_state->num_players = 0;
    game_state->game_reset_requested = 0;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game_state->players[i].state = PLAYER_DISCONNECTED;
        game_state->players[i].score = 0;
        memset(game_state->players[i].name, 0, MAX_NAME_LEN);
    }
    
    pthread_mutex_unlock(&game_state->game_mutex);
    
    enqueue_log("Game state reset for new game");
}

// ============================================================================
// Main Server Loop
// ============================================================================

int main(void) {
    printf("========================================\n");
    printf("   RACE TO 100 - Game Server\n");
    printf("========================================\n");
    printf("Goal: First player to reach %d points wins!\n", WINNING_SCORE);
    printf("Rules: Roll two dice, add to your score.\n");
    printf("       Snake eyes (1+1) resets your score!\n");
    printf("Players needed: %d to %d\n", MIN_PLAYERS, MAX_PLAYERS);
    printf("========================================\n\n");
    
    // Setup signal handlers
    struct sigaction sa_chld, sa_int;
    
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);
    
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGTERM, &sa_int, NULL);
    
    // Setup shared memory
    if (setup_shared_memory() < 0) {
        fprintf(stderr, "Failed to setup shared memory\n");
        return 1;
    }
    printf("[Server] Shared memory initialized\n");
    
    // Load persistent scores
    load_scores();
    
    // Create named pipes for each potential player slot
    for (int i = 0; i < MAX_PLAYERS; i++) {
        char pipe_name[64];
        
        snprintf(pipe_name, sizeof(pipe_name), "%s%d_to_server", PIPE_BASE, i);
        unlink(pipe_name); // Remove if exists
        if (mkfifo(pipe_name, 0666) < 0 && errno != EEXIST) {
            perror("mkfifo");
            cleanup_shared_memory();
            return 1;
        }
        
        snprintf(pipe_name, sizeof(pipe_name), "%s%d_to_client", PIPE_BASE, i);
        unlink(pipe_name);
        if (mkfifo(pipe_name, 0666) < 0 && errno != EEXIST) {
            perror("mkfifo");
            cleanup_shared_memory();
            return 1;
        }
    }
    printf("[Server] Named pipes created\n");
    
    // Start logger thread
    if (pthread_create(&logger_thread, NULL, logger_thread_func, NULL) != 0) {
        perror("pthread_create logger");
        cleanup_shared_memory();
        return 1;
    }
    printf("[Server] Logger thread created\n");
    
    // Start scheduler thread
    if (pthread_create(&scheduler_thread, NULL, scheduler_thread_func, NULL) != 0) {
        perror("pthread_create scheduler");
        cleanup_shared_memory();
        return 1;
    }
    printf("[Server] Scheduler thread created\n");
    
    enqueue_log("=== SERVER STARTED ===");
    enqueue_log("Waiting for players to connect...");
    
    printf("\n[Server] Ready! Waiting for players to connect...\n");
    printf("[Server] Press Ctrl+C to shutdown\n\n");
    
    // Main accept loop
    int current_slot = 0;
    
    while (server_running) {
        // Find next available player slot
        pthread_mutex_lock(&game_state->game_mutex);
        int slot = -1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (game_state->players[i].state == PLAYER_DISCONNECTED) {
                slot = i;
                break;
            }
        }
        int game_finished = (game_state->game_state == GAME_FINISHED);
        pthread_mutex_unlock(&game_state->game_mutex);
        
        // Handle game reset for multi-game support
        if (game_finished) {
            printf("[Server] Game finished. Resetting for new game...\n");
            sleep(3); // Brief pause before reset
            reset_game_state();
            continue;
        }
        
        if (slot < 0) {
            // No slots available, wait
            usleep(100000);
            continue;
        }
        
        char pipe_to_server[64], pipe_to_client[64];
        snprintf(pipe_to_server, sizeof(pipe_to_server), "%s%d_to_server", PIPE_BASE, slot);
        snprintf(pipe_to_client, sizeof(pipe_to_client), "%s%d_to_client", PIPE_BASE, slot);
        
        printf("[Server] Waiting for player on slot %d...\n", slot + 1);
        
        // Open pipes (blocking until client connects)
        int read_fd = open(pipe_to_server, O_RDONLY);
        if (read_fd < 0) {
            if (errno == EINTR) continue;
            perror("open pipe read");
            continue;
        }
        
        int write_fd = open(pipe_to_client, O_WRONLY);
        if (write_fd < 0) {
            close(read_fd);
            if (errno == EINTR) continue;
            perror("open pipe write");
            continue;
        }
        
        printf("[Server] Client connected on slot %d\n", slot + 1);
        enqueue_log("Client connected on slot %d", slot + 1);
        
        // Fork child process to handle this client
        pid_t pid = fork();
        
        if (pid < 0) {
            perror("fork");
            close(read_fd);
            close(write_fd);
            continue;
        } else if (pid == 0) {
            // Child process - handle client
            handle_client(slot, read_fd, write_fd);
            exit(0); // Should never reach here
        } else {
            // Parent process
            game_state->players[slot].handler_pid = pid;
            close(read_fd);  // Parent doesn't need these
            close(write_fd);
            printf("[Server] Forked handler process %d for slot %d\n", pid, slot + 1);
        }
        
        current_slot = (slot + 1) % MAX_PLAYERS;
    }
    
    // Shutdown sequence
    printf("\n[Server] Shutting down...\n");
    
    // Wait for threads
    pthread_join(scheduler_thread, NULL);
    pthread_join(logger_thread, NULL);
    
    // Cleanup
    cleanup_shared_memory();
    
    printf("[Server] Goodbye!\n");
    return 0;
}
