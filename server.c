/*
 * Concurrent Networked Sudoku Game Server
 * Collaborative Sudoku - Players compete to solve a shared puzzle
 * 
 * Compatible with Minix and minimal POSIX systems
 * Single-file version - no external headers needed
 * 
 * Compile: gcc -o server server.c -lpthread
 * Run: ./server
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

// ============================================================================
// Configuration & Constants (from common.h)
// ============================================================================

#define SHM_KEY_GAME   0x5355444F  // "SUDO"
#define SHM_KEY_LOG    0x4C4F4753  // "LOGS"
#define SHM_KEY_SCORE  0x53434F52  // "SCOR"

#define MIN_PLAYERS 3
#define MAX_PLAYERS 5
#define MAX_NAME_LEN 32
#define MAX_LOG_MSG 256
#define LOG_QUEUE_SIZE 100
#define SCORES_FILE "sudoku_scores.txt"
#define LOG_FILE "sudoku_game.log"
#define MAX_SCORES 100

#define GRID_SIZE 9
#define BOX_SIZE 3
#define EMPTY_CELL 0

#define POINTS_CORRECT 10
#define POINTS_WRONG -5

#define PIPE_BASE "/tmp/sudoku_pipe_"

// ============================================================================
// Type Definitions (from common.h)
// ============================================================================

typedef enum {
    PLAYER_DISCONNECTED = 0,
    PLAYER_WAITING,
    PLAYER_ACTIVE,
    PLAYER_FINISHED
} PlayerState;

typedef enum {
    GAME_WAITING_FOR_PLAYERS = 0,
    GAME_IN_PROGRESS,
    GAME_FINISHED
} GameState;

typedef struct {
    int id;
    char name[MAX_NAME_LEN];
    int score;
    int correct_placements;
    int wrong_placements;
    PlayerState state;
    pid_t handler_pid;
} Player;

typedef struct {
    volatile int lock;
} SpinLock;

typedef struct {
    int value;
    int solution;
    int is_fixed;
    int placed_by;
} SudokuCell;

typedef struct {
    GameState game_state;
    int num_players;
    int current_turn;
    int winner_id;
    SudokuCell grid[GRID_SIZE][GRID_SIZE];
    int cells_remaining;
    Player players[MAX_PLAYERS];
    SpinLock game_lock;
    volatile int turn_signal;
    volatile int game_reset_requested;
    volatile int server_shutdown;
} SharedGameState;

typedef struct {
    char message[MAX_LOG_MSG];
    time_t timestamp;
} LogEntry;

typedef struct {
    LogEntry entries[LOG_QUEUE_SIZE];
    int head;
    int tail;
    volatile int count;
    SpinLock lock;
    volatile int shutdown;
} LogQueue;

typedef struct {
    char name[MAX_NAME_LEN];
    int wins;
    int total_correct;
    int total_wrong;
} ScoreEntry;

typedef struct {
    ScoreEntry entries[MAX_SCORES];
    int count;
    SpinLock lock;
} SharedScores;

typedef enum {
    MSG_JOIN = 1,
    MSG_PLACE,
    MSG_QUIT,
    MSG_GAME_STATE,
    MSG_YOUR_TURN,
    MSG_PLACE_RESULT,
    MSG_GAME_OVER,
    MSG_WAIT,
    MSG_ERROR,
    MSG_PLAYER_JOINED,
    MSG_PLAYER_LEFT,
    MSG_GAME_START,
    MSG_GRID_UPDATE
} MessageType;

typedef struct {
    MessageType type;
    int player_id;
    char player_name[MAX_NAME_LEN];
    int row;
    int col;
    int value;
    int success;
    int points_earned;
    char text[MAX_LOG_MSG];
    SudokuCell grid[GRID_SIZE][GRID_SIZE];
    int cells_remaining;
    Player players[MAX_PLAYERS];
    int num_players;
    int current_turn;
} GameMessage;

// ============================================================================
// Spinlock Functions
// ============================================================================

static inline void spin_lock_init(SpinLock *lock) {
    lock->lock = 0;
}

static inline void spin_lock(SpinLock *lock) {
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
        usleep(100);
    }
}

static inline void spin_unlock(SpinLock *lock) {
    __sync_lock_release(&lock->lock);
}

// ============================================================================
// Global Variables
// ============================================================================

SharedGameState *game_state = NULL;
LogQueue *log_queue = NULL;
SharedScores *scores = NULL;

pthread_t scheduler_thread;
pthread_t logger_thread;

volatile sig_atomic_t server_running = 1;

int shm_game_id = -1;
int shm_log_id = -1;
int shm_scores_id = -1;

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
    
    spin_lock(&log_queue->lock);
    
    if (log_queue->count < LOG_QUEUE_SIZE) {
        LogEntry *entry = &log_queue->entries[log_queue->tail];
        strncpy(entry->message, message, MAX_LOG_MSG - 1);
        entry->message[MAX_LOG_MSG - 1] = '\0';
        entry->timestamp = time(NULL);
        
        log_queue->tail = (log_queue->tail + 1) % LOG_QUEUE_SIZE;
        log_queue->count++;
    }
    
    spin_unlock(&log_queue->lock);
}

void *logger_thread_func(void *arg) {
    (void)arg;
    FILE *log_file = fopen(LOG_FILE, "a");
    if (!log_file) {
        perror("Failed to open log file");
        return NULL;
    }
    
    setvbuf(log_file, NULL, _IOLBF, 0);
    printf("[Logger] Logger thread started\n");
    
    while (1) {
        int should_exit = 0;
        
        spin_lock(&log_queue->lock);
        
        while (log_queue->count > 0) {
            LogEntry *entry = &log_queue->entries[log_queue->head];
            
            struct tm *tm_info = localtime(&entry->timestamp);
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            
            fprintf(log_file, "[%s] %s\n", time_str, entry->message);
            fflush(log_file);
            
            log_queue->head = (log_queue->head + 1) % LOG_QUEUE_SIZE;
            log_queue->count--;
        }
        
        should_exit = log_queue->shutdown && log_queue->count == 0;
        spin_unlock(&log_queue->lock);
        
        if (should_exit) break;
        usleep(50000);
    }
    
    fclose(log_file);
    printf("[Logger] Logger thread terminated\n");
    return NULL;
}

// ============================================================================
// Sudoku Generation and Validation
// ============================================================================

int is_valid_placement(int grid[GRID_SIZE][GRID_SIZE], int row, int col, int num) {
    for (int c = 0; c < GRID_SIZE; c++) {
        if (grid[row][c] == num) return 0;
    }
    
    for (int r = 0; r < GRID_SIZE; r++) {
        if (grid[r][col] == num) return 0;
    }
    
    int box_row = (row / BOX_SIZE) * BOX_SIZE;
    int box_col = (col / BOX_SIZE) * BOX_SIZE;
    for (int r = box_row; r < box_row + BOX_SIZE; r++) {
        for (int c = box_col; c < box_col + BOX_SIZE; c++) {
            if (grid[r][c] == num) return 0;
        }
    }
    
    return 1;
}

void shuffle_array(int *arr, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = arr[i];
        arr[i] = arr[j];
        arr[j] = temp;
    }
}

int generate_full_grid(int grid[GRID_SIZE][GRID_SIZE]) {
    for (int row = 0; row < GRID_SIZE; row++) {
        for (int col = 0; col < GRID_SIZE; col++) {
            if (grid[row][col] == EMPTY_CELL) {
                int nums[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
                shuffle_array(nums, 9);
                
                for (int i = 0; i < 9; i++) {
                    if (is_valid_placement(grid, row, col, nums[i])) {
                        grid[row][col] = nums[i];
                        if (generate_full_grid(grid)) {
                            return 1;
                        }
                        grid[row][col] = EMPTY_CELL;
                    }
                }
                return 0;
            }
        }
    }
    return 1;
}

void generate_puzzle(SharedGameState *state, int difficulty) {
    int solution[GRID_SIZE][GRID_SIZE] = {0};
    
    generate_full_grid(solution);
    
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            state->grid[r][c].solution = solution[r][c];
            state->grid[r][c].value = solution[r][c];
            state->grid[r][c].is_fixed = 1;
            state->grid[r][c].placed_by = -1;
        }
    }
    
    int cells_to_remove = 30 + (difficulty * 5);
    if (cells_to_remove > 55) cells_to_remove = 55;
    
    int removed = 0;
    while (removed < cells_to_remove) {
        int row = rand() % GRID_SIZE;
        int col = rand() % GRID_SIZE;
        
        if (state->grid[row][col].value != EMPTY_CELL) {
            state->grid[row][col].value = EMPTY_CELL;
            state->grid[row][col].is_fixed = 0;
            removed++;
        }
    }
    
    state->cells_remaining = cells_to_remove;
    enqueue_log("Generated puzzle with %d empty cells (difficulty: %d)", cells_to_remove, difficulty);
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
    
    spin_lock(&scores->lock);
    
    char name[MAX_NAME_LEN];
    int wins, correct, wrong;
    while (fscanf(file, "%31s %d %d %d", name, &wins, &correct, &wrong) == 4 && 
           scores->count < MAX_SCORES) {
        strncpy(scores->entries[scores->count].name, name, MAX_NAME_LEN - 1);
        scores->entries[scores->count].wins = wins;
        scores->entries[scores->count].total_correct = correct;
        scores->entries[scores->count].total_wrong = wrong;
        scores->count++;
    }
    
    spin_unlock(&scores->lock);
    fclose(file);
    
    enqueue_log("Loaded %d score entries from %s", scores->count, SCORES_FILE);
}

void save_scores(void) {
    FILE *file = fopen(SCORES_FILE, "w");
    if (!file) {
        perror("Failed to save scores");
        return;
    }
    
    spin_lock(&scores->lock);
    
    for (int i = 0; i < scores->count; i++) {
        fprintf(file, "%s %d %d %d\n", 
                scores->entries[i].name, 
                scores->entries[i].wins,
                scores->entries[i].total_correct,
                scores->entries[i].total_wrong);
    }
    
    spin_unlock(&scores->lock);
    fclose(file);
}

void update_player_stats(const char *player_name, int is_winner, int correct, int wrong) {
    spin_lock(&scores->lock);
    
    int found = -1;
    for (int i = 0; i < scores->count; i++) {
        if (strcmp(scores->entries[i].name, player_name) == 0) {
            found = i;
            break;
        }
    }
    
    if (found >= 0) {
        if (is_winner) scores->entries[found].wins++;
        scores->entries[found].total_correct += correct;
        scores->entries[found].total_wrong += wrong;
    } else if (scores->count < MAX_SCORES) {
        strncpy(scores->entries[scores->count].name, player_name, MAX_NAME_LEN - 1);
        scores->entries[scores->count].wins = is_winner ? 1 : 0;
        scores->entries[scores->count].total_correct = correct;
        scores->entries[scores->count].total_wrong = wrong;
        scores->count++;
    }
    
    spin_unlock(&scores->lock);
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
    
    return -1;
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
    enqueue_log("Round Robin Scheduler initialized for Sudoku");
    
    while (server_running) {
        spin_lock(&game_state->game_lock);
        
        if (game_state->game_state == GAME_IN_PROGRESS) {
            int current = game_state->current_turn;
            
            if (current >= 0 && current < MAX_PLAYERS) {
                if (game_state->players[current].state != PLAYER_ACTIVE) {
                    int next = get_next_active_player(current);
                    if (next >= 0) {
                        game_state->current_turn = next;
                        game_state->turn_signal++;
                        enqueue_log("Scheduler: Turn passed to Player %d (%s)",
                                   next + 1, game_state->players[next].name);
                    } else {
                        game_state->game_state = GAME_FINISHED;
                        enqueue_log("Scheduler: No active players, game ended");
                    }
                }
            }
            
            if (game_state->cells_remaining <= 0) {
                game_state->game_state = GAME_FINISHED;
                
                int max_score = -1000;
                int winner = -1;
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (game_state->players[i].state == PLAYER_ACTIVE &&
                        game_state->players[i].score > max_score) {
                        max_score = game_state->players[i].score;
                        winner = i;
                    }
                }
                game_state->winner_id = winner;
                
                if (winner >= 0) {
                    enqueue_log("PUZZLE COMPLETE! Winner: Player %d (%s) with %d points!",
                               winner + 1, game_state->players[winner].name, max_score);
                    
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        if (game_state->players[i].state == PLAYER_ACTIVE) {
                            update_player_stats(game_state->players[i].name,
                                              i == winner,
                                              game_state->players[i].correct_placements,
                                              game_state->players[i].wrong_placements);
                        }
                    }
                }
            }
        }
        
        spin_unlock(&game_state->game_lock);
        usleep(50000);
    }
    
    printf("[Scheduler] Scheduler thread terminated\n");
    return NULL;
}

void advance_turn(void) {
    spin_lock(&game_state->game_lock);
    
    // Pick the next active player in round-robin order.
    // We keep it simple: find next connected/active player after current_turn.
    int next = get_next_active_player(game_state->current_turn);
    if (next >= 0) {
        game_state->current_turn = next;
        // turn_signal is a simple counter used by other parts of the program
        // to notice that the turn changed.
        game_state->turn_signal++;
        enqueue_log("Turn advanced to Player %d (%s)", 
                   next + 1, game_state->players[next].name);
    }
    
    spin_unlock(&game_state->game_lock);
}

// ============================================================================
// Helper: Copy game state to message
// ============================================================================

void copy_state_to_message(GameMessage *msg) {
    // Copy the current shared state into a message so the client can redraw
    // the grid and scoreboard from one packet.
    memcpy(msg->grid, game_state->grid, sizeof(game_state->grid));
    memcpy(msg->players, game_state->players, sizeof(game_state->players));
    msg->cells_remaining = game_state->cells_remaining;
    msg->num_players = game_state->num_players;
    msg->current_turn = game_state->current_turn;
}

// ============================================================================
// Broadcast grid update to all active clients
// ============================================================================

void broadcast_grid_update(int exclude_player_id, int row, int col, int value, int success, const char *player_name) {
    // This is used so OTHER clients update automatically when someone plays.
    // We exclude the player who just played because they already receive a direct response.
    GameMessage update;
    char pipe_to_client[64];
    
    memset(&update, 0, sizeof(GameMessage));
    update.type = MSG_GRID_UPDATE;
    update.row = row;
    update.col = col;
    update.value = value;
    update.success = success;
    copy_state_to_message(&update);
    
    if (success) {
        snprintf(update.text, MAX_LOG_MSG, 
                "Player %s placed %d at (%d,%d) - CORRECT!",
                player_name, value, row + 1, col + 1);
    } else {
        snprintf(update.text, MAX_LOG_MSG, 
                "Player %s tried %d at (%d,%d) - WRONG!",
                player_name, value, row + 1, col + 1);
    }
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == exclude_player_id) continue;
        if (game_state->players[i].state != PLAYER_ACTIVE) continue;
        
        snprintf(pipe_to_client, sizeof(pipe_to_client), "%s%d_to_client", PIPE_BASE, i);
        // Non-blocking open: if the client is not ready / disconnected,
        // we don't want the server to hang here.
        int fd = open(pipe_to_client, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            ssize_t written = write(fd, &update, sizeof(GameMessage));
            close(fd);
            // Ignore write errors - client may have disconnected.
            // (This is a class project; best-effort broadcast is OK.)
        }
    }
}

// ============================================================================
// Broadcast turn notification to all players
// ============================================================================

void broadcast_turn_notification(void) {
    // Purpose:
    // - Tell the current player "YOUR TURN"
    // - Tell everyone else who should play now
    //
    // This solves the "who's turn is it?" confusion in multiple terminals.
    GameMessage turn_msg;
    char pipe_to_client[64];
    
    spin_lock(&game_state->game_lock);
    
    if (game_state->game_state != GAME_IN_PROGRESS) {
        spin_unlock(&game_state->game_lock);
        return;
    }
    
    int current_turn = game_state->current_turn;
    if (current_turn < 0 || current_turn >= MAX_PLAYERS) {
        spin_unlock(&game_state->game_lock);
        return;
    }
    
    const char *current_player_name = game_state->players[current_turn].name;
    copy_state_to_message(&turn_msg);
    
    // Send "Your turn" message to the current player
    turn_msg.type = MSG_YOUR_TURN;
    snprintf(turn_msg.text, MAX_LOG_MSG, 
            ">>> IT'S YOUR TURN, %s! Use 'place R C N' to place a number.",
            current_player_name);
    
    snprintf(pipe_to_client, sizeof(pipe_to_client), "%s%d_to_client", PIPE_BASE, current_turn);
    int fd = open(pipe_to_client, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        write(fd, &turn_msg, sizeof(GameMessage));
        close(fd);
    }
    
    // Send "It's Player X's turn" message to other players
    turn_msg.type = MSG_WAIT;
    snprintf(turn_msg.text, MAX_LOG_MSG, 
            "It's %s's turn (Player %d). Please wait...",
            current_player_name, current_turn + 1);
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == current_turn) continue;
        if (game_state->players[i].state != PLAYER_ACTIVE) continue;
        
        snprintf(pipe_to_client, sizeof(pipe_to_client), "%s%d_to_client", PIPE_BASE, i);
        fd = open(pipe_to_client, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            write(fd, &turn_msg, sizeof(GameMessage));
            close(fd);
        }
    }
    
    spin_unlock(&game_state->game_lock);
}

// ============================================================================
// Client Handler (Child Process)
// ============================================================================

void handle_client(int player_id, int pipe_read_fd, int pipe_write_fd) {
    Player *player = &game_state->players[player_id];
    GameMessage msg, response;
    
    printf("[Handler %d] Started for player %d\n", getpid(), player_id + 1);
    enqueue_log("Handler process started for Player %d", player_id + 1);
    
    srand(time(NULL) ^ getpid());
    
    while (1) {
        ssize_t bytes = read(pipe_read_fd, &msg, sizeof(GameMessage));
        if (bytes <= 0) {
            spin_lock(&game_state->game_lock);
            player->state = PLAYER_DISCONNECTED;
            game_state->num_players--;
            spin_unlock(&game_state->game_lock);
            
            enqueue_log("Player %d (%s) disconnected", player_id + 1, player->name);
            break;
        }
        
        memset(&response, 0, sizeof(GameMessage));
        
        switch (msg.type) {
            case MSG_JOIN: {
                spin_lock(&game_state->game_lock);
                
                strncpy(player->name, msg.player_name, MAX_NAME_LEN - 1);
                player->state = PLAYER_WAITING;
                player->score = 0;
                player->correct_placements = 0;
                player->wrong_placements = 0;
                game_state->num_players++;
                
                response.type = MSG_PLAYER_JOINED;
                response.player_id = player_id;
                copy_state_to_message(&response);
                snprintf(response.text, MAX_LOG_MSG, 
                        "Welcome %s! You are Player %d. Waiting for %d more players...",
                        player->name, player_id + 1, 
                        MIN_PLAYERS - game_state->num_players);
                
                enqueue_log("Player %d joined: %s (Total: %d players)", 
                           player_id + 1, player->name, game_state->num_players);
                
                if (game_state->num_players >= MIN_PLAYERS && 
                    game_state->game_state == GAME_WAITING_FOR_PLAYERS) {
                    
                    generate_puzzle(game_state, 2);
                    
                    game_state->game_state = GAME_IN_PROGRESS;
                    
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        if (game_state->players[i].state == PLAYER_WAITING) {
                            game_state->players[i].state = PLAYER_ACTIVE;
                        }
                    }
                    game_state->current_turn = get_next_active_player(-1);
                    game_state->turn_signal++;
                    
                    copy_state_to_message(&response);
                    response.type = MSG_GAME_START;
                    snprintf(response.text, MAX_LOG_MSG, 
                            "Game started! %d cells to fill. First turn: Player %d",
                            game_state->cells_remaining, game_state->current_turn + 1);
                    
                    enqueue_log("Game started with %d players! %d cells to fill",
                               game_state->num_players, game_state->cells_remaining);
                }
                
                spin_unlock(&game_state->game_lock);
                write(pipe_write_fd, &response, sizeof(GameMessage));
                
                // If game just started, notify players about whose turn it is
                if (game_state->game_state == GAME_IN_PROGRESS && 
                    game_state->current_turn >= 0) {
                    broadcast_turn_notification();
                }
                break;
            }
            
            case MSG_PLACE: {
                spin_lock(&game_state->game_lock);
                
                if (game_state->game_state != GAME_IN_PROGRESS) {
                    response.type = MSG_ERROR;
                    snprintf(response.text, MAX_LOG_MSG, "Game not in progress");
                    spin_unlock(&game_state->game_lock);
                    write(pipe_write_fd, &response, sizeof(GameMessage));
                    break;
                }
                
                if (game_state->current_turn != player_id) {
                    response.type = MSG_WAIT;
                    copy_state_to_message(&response);
                    snprintf(response.text, MAX_LOG_MSG, 
                            "Not your turn! Current turn: Player %d (%s)",
                            game_state->current_turn + 1,
                            game_state->players[game_state->current_turn].name);
                    spin_unlock(&game_state->game_lock);
                    write(pipe_write_fd, &response, sizeof(GameMessage));
                    break;
                }
                
                int row = msg.row;
                int col = msg.col;
                int value = msg.value;
                
                if (row < 0 || row >= GRID_SIZE || col < 0 || col >= GRID_SIZE) {
                    response.type = MSG_ERROR;
                    snprintf(response.text, MAX_LOG_MSG, "Invalid position (%d,%d)", row + 1, col + 1);
                    spin_unlock(&game_state->game_lock);
                    write(pipe_write_fd, &response, sizeof(GameMessage));
                    break;
                }
                
                if (value < 1 || value > 9) {
                    response.type = MSG_ERROR;
                    snprintf(response.text, MAX_LOG_MSG, "Invalid number %d (must be 1-9)", value);
                    spin_unlock(&game_state->game_lock);
                    write(pipe_write_fd, &response, sizeof(GameMessage));
                    break;
                }
                
                SudokuCell *cell = &game_state->grid[row][col];
                
                if (cell->is_fixed) {
                    response.type = MSG_ERROR;
                    snprintf(response.text, MAX_LOG_MSG, "Cell (%d,%d) is fixed and cannot be changed", row + 1, col + 1);
                    spin_unlock(&game_state->game_lock);
                    write(pipe_write_fd, &response, sizeof(GameMessage));
                    break;
                }
                
                if (cell->value != EMPTY_CELL) {
                    response.type = MSG_ERROR;
                    snprintf(response.text, MAX_LOG_MSG, "Cell (%d,%d) already has value %d", row + 1, col + 1, cell->value);
                    spin_unlock(&game_state->game_lock);
                    write(pipe_write_fd, &response, sizeof(GameMessage));
                    break;
                }
                
                response.type = MSG_PLACE_RESULT;
                response.row = row;
                response.col = col;
                response.value = value;
                
                if (value == cell->solution) {
                    cell->value = value;
                    cell->placed_by = player_id;
                    game_state->cells_remaining--;
                    
                    player->score += POINTS_CORRECT;
                    player->correct_placements++;
                    
                    response.success = 1;
                    response.points_earned = POINTS_CORRECT;
                    snprintf(response.text, MAX_LOG_MSG, 
                            "CORRECT! +%d points. Score: %d. Cells remaining: %d",
                            POINTS_CORRECT, player->score, game_state->cells_remaining);
                    
                    enqueue_log("Player %d (%s) placed %d at (%d,%d) - CORRECT! Score: %d",
                               player_id + 1, player->name, value, row + 1, col + 1, player->score);
                } else {
                    player->score += POINTS_WRONG;
                    player->wrong_placements++;
                    
                    response.success = 0;
                    response.points_earned = POINTS_WRONG;
                    snprintf(response.text, MAX_LOG_MSG, 
                            "WRONG! %d points. Score: %d. Try again next turn!",
                            POINTS_WRONG, player->score);
                    
                    enqueue_log("Player %d (%s) placed %d at (%d,%d) - WRONG! Score: %d",
                               player_id + 1, player->name, value, row + 1, col + 1, player->score);
                }
                
                copy_state_to_message(&response);
                
                if (game_state->cells_remaining <= 0) {
                    response.type = MSG_GAME_OVER;
                    
                    int max_score = -1000;
                    int winner = -1;
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        if (game_state->players[i].state == PLAYER_ACTIVE &&
                            game_state->players[i].score > max_score) {
                            max_score = game_state->players[i].score;
                            winner = i;
                        }
                    }
                    game_state->winner_id = winner;
                    game_state->game_state = GAME_FINISHED;
                    
                    if (winner == player_id) {
                        snprintf(response.text, MAX_LOG_MSG,
                                "PUZZLE COMPLETE! CONGRATULATIONS - YOU WON with %d points!",
                                player->score);
                    } else if (winner >= 0) {
                        snprintf(response.text, MAX_LOG_MSG,
                                "PUZZLE COMPLETE! Winner: %s with %d points. Your score: %d",
                                game_state->players[winner].name, max_score, player->score);
                    }
                    
                    spin_unlock(&game_state->game_lock);
                    write(pipe_write_fd, &response, sizeof(GameMessage));
                    break;
                }
                
                spin_unlock(&game_state->game_lock);
                advance_turn();
                write(pipe_write_fd, &response, sizeof(GameMessage));
                
                // Broadcast update to all other clients
                broadcast_grid_update(player_id, row, col, value, response.success, player->name);
                
                // Broadcast turn notification to all players
                broadcast_turn_notification();
                break;
            }
            
            case MSG_GAME_STATE: {
                spin_lock(&game_state->game_lock);
                
                response.type = MSG_GAME_STATE;
                copy_state_to_message(&response);
                
                snprintf(response.text, MAX_LOG_MSG, 
                        "Game: %s | Cells left: %d | Your turn: %s",
                        game_state->game_state == GAME_WAITING_FOR_PLAYERS ? "Waiting" :
                        game_state->game_state == GAME_IN_PROGRESS ? "In Progress" : "Finished",
                        game_state->cells_remaining,
                        game_state->current_turn == player_id ? "YES" : "NO");
                
                spin_unlock(&game_state->game_lock);
                write(pipe_write_fd, &response, sizeof(GameMessage));
                break;
            }
            
            case MSG_QUIT: {
                spin_lock(&game_state->game_lock);
                player->state = PLAYER_DISCONNECTED;
                game_state->num_players--;
                spin_unlock(&game_state->game_lock);
                
                enqueue_log("Player %d (%s) quit the game", player_id + 1, player->name);
                
                response.type = MSG_PLAYER_LEFT;
                snprintf(response.text, MAX_LOG_MSG, "Goodbye %s! Final score: %d", 
                        player->name, player->score);
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
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

void sigint_handler(int sig) {
    (void)sig;
    printf("\n[Server] Shutdown signal received...\n");
    server_running = 0;
    save_scores();
    
    if (log_queue) {
        spin_lock(&log_queue->lock);
        log_queue->shutdown = 1;
        spin_unlock(&log_queue->lock);
    }
}

// ============================================================================
// Shared Memory Setup
// ============================================================================

int setup_shared_memory(void) {
    shm_game_id = shmget(SHM_KEY_GAME, sizeof(SharedGameState), IPC_CREAT | 0666);
    if (shm_game_id < 0) {
        perror("shmget game state");
        return -1;
    }
    game_state = (SharedGameState *)shmat(shm_game_id, NULL, 0);
    if (game_state == (void *)-1) {
        perror("shmat game state");
        return -1;
    }
    
    shm_log_id = shmget(SHM_KEY_LOG, sizeof(LogQueue), IPC_CREAT | 0666);
    if (shm_log_id < 0) {
        perror("shmget log queue");
        return -1;
    }
    log_queue = (LogQueue *)shmat(shm_log_id, NULL, 0);
    if (log_queue == (void *)-1) {
        perror("shmat log queue");
        return -1;
    }
    
    shm_scores_id = shmget(SHM_KEY_SCORE, sizeof(SharedScores), IPC_CREAT | 0666);
    if (shm_scores_id < 0) {
        perror("shmget scores");
        return -1;
    }
    scores = (SharedScores *)shmat(shm_scores_id, NULL, 0);
    if (scores == (void *)-1) {
        perror("shmat scores");
        return -1;
    }
    
    memset(game_state, 0, sizeof(SharedGameState));
    spin_lock_init(&game_state->game_lock);
    game_state->game_state = GAME_WAITING_FOR_PLAYERS;
    game_state->current_turn = -1;
    game_state->winner_id = -1;
    game_state->cells_remaining = 0;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game_state->players[i].id = i;
        game_state->players[i].state = PLAYER_DISCONNECTED;
        game_state->players[i].score = 0;
        game_state->players[i].correct_placements = 0;
        game_state->players[i].wrong_placements = 0;
    }
    
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            game_state->grid[r][c].value = EMPTY_CELL;
            game_state->grid[r][c].solution = 0;
            game_state->grid[r][c].is_fixed = 0;
            game_state->grid[r][c].placed_by = -1;
        }
    }
    
    memset(log_queue, 0, sizeof(LogQueue));
    spin_lock_init(&log_queue->lock);
    
    memset(scores, 0, sizeof(SharedScores));
    spin_lock_init(&scores->lock);
    
    printf("[Server] System V shared memory initialized\n");
    return 0;
}

void cleanup_shared_memory(void) {
    if (game_state) shmdt(game_state);
    if (log_queue) shmdt(log_queue);
    if (scores) shmdt(scores);
    
    if (shm_game_id >= 0) shmctl(shm_game_id, IPC_RMID, NULL);
    if (shm_log_id >= 0) shmctl(shm_log_id, IPC_RMID, NULL);
    if (shm_scores_id >= 0) shmctl(shm_scores_id, IPC_RMID, NULL);
    
    printf("[Server] Shared memory cleaned up\n");
}

// ============================================================================
// Named Pipe Setup
// ============================================================================

int setup_named_pipes(void) {
    char pipe_to_server[64], pipe_to_client[64];
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        snprintf(pipe_to_server, sizeof(pipe_to_server), "%s%d_to_server", PIPE_BASE, i);
        snprintf(pipe_to_client, sizeof(pipe_to_client), "%s%d_to_client", PIPE_BASE, i);
        
        unlink(pipe_to_server);
        unlink(pipe_to_client);
        
        if (mkfifo(pipe_to_server, 0666) < 0 && errno != EEXIST) {
            perror("mkfifo to_server");
            return -1;
        }
        if (mkfifo(pipe_to_client, 0666) < 0 && errno != EEXIST) {
            perror("mkfifo to_client");
            return -1;
        }
    }
    
    printf("[Server] Named pipes created\n");
    return 0;
}

void cleanup_named_pipes(void) {
    char pipe_path[64];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        snprintf(pipe_path, sizeof(pipe_path), "%s%d_to_server", PIPE_BASE, i);
        unlink(pipe_path);
        snprintf(pipe_path, sizeof(pipe_path), "%s%d_to_client", PIPE_BASE, i);
        unlink(pipe_path);
    }
    printf("[Server] Named pipes cleaned up\n");
}

// ============================================================================
// Player Connection Handler
// ============================================================================

void accept_player_connections(void) {
    char pipe_to_server[64], pipe_to_client[64];
    
    printf("[Server] Waiting for player connections...\n");
    printf("[Server] Players can connect to slots 0-%d\n", MAX_PLAYERS - 1);
    
    for (int i = 0; i < MAX_PLAYERS && server_running; i++) {
        snprintf(pipe_to_server, sizeof(pipe_to_server), "%s%d_to_server", PIPE_BASE, i);
        snprintf(pipe_to_client, sizeof(pipe_to_client), "%s%d_to_client", PIPE_BASE, i);
        
        printf("[Server] Opening pipes for slot %d...\n", i);
        
        int fd_read = open(pipe_to_server, O_RDONLY);
        if (fd_read < 0) {
            if (errno == EINTR) continue;
            perror("open pipe_to_server");
            continue;
        }
        
        int fd_write = open(pipe_to_client, O_WRONLY);
        if (fd_write < 0) {
            close(fd_read);
            perror("open pipe_to_client");
            continue;
        }
        
        printf("[Server] Client connected to slot %d\n", i);
        enqueue_log("Client connected to slot %d", i);
        
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(fd_read);
            close(fd_write);
            continue;
        }
        
        if (pid == 0) {
            handle_client(i, fd_read, fd_write);
            exit(0);
        } else {
            game_state->players[i].handler_pid = pid;
            close(fd_read);
            close(fd_write);
        }
    }
}

// ============================================================================
// Main Function
// ============================================================================

int main(void) {
    printf("======================================\n");
    printf("  COLLABORATIVE SUDOKU GAME SERVER\n");
    printf("  Minix/Mini OS Compatible Version\n");
    printf("======================================\n\n");
    
    srand(time(NULL));
    
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
    
    if (setup_shared_memory() < 0) {
        fprintf(stderr, "Failed to setup shared memory\n");
        return 1;
    }
    
    if (setup_named_pipes() < 0) {
        fprintf(stderr, "Failed to setup named pipes\n");
        cleanup_shared_memory();
        return 1;
    }
    
    load_scores();
    
    if (pthread_create(&logger_thread, NULL, logger_thread_func, NULL) != 0) {
        perror("pthread_create logger");
        cleanup_named_pipes();
        cleanup_shared_memory();
        return 1;
    }
    
    if (pthread_create(&scheduler_thread, NULL, scheduler_thread_func, NULL) != 0) {
        perror("pthread_create scheduler");
        log_queue->shutdown = 1;
        pthread_join(logger_thread, NULL);
        cleanup_named_pipes();
        cleanup_shared_memory();
        return 1;
    }
    
    enqueue_log("=== SUDOKU SERVER STARTED ===");
    
    printf("[Server] Server initialized successfully!\n");
    printf("[Server] Waiting for %d-%d players to connect...\n", MIN_PLAYERS, MAX_PLAYERS);
    printf("[Server] Press Ctrl+C to shutdown\n\n");
    
    accept_player_connections();
    
    while (server_running) {
        sleep(1);
    }
    
    printf("\n[Server] Shutting down...\n");
    
    enqueue_log("=== SERVER SHUTDOWN ===");
    save_scores();
    
    spin_lock(&log_queue->lock);
    log_queue->shutdown = 1;
    spin_unlock(&log_queue->lock);
    
    pthread_join(scheduler_thread, NULL);
    pthread_join(logger_thread, NULL);
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game_state->players[i].handler_pid > 0) {
            kill(game_state->players[i].handler_pid, SIGTERM);
        }
    }
    
    cleanup_named_pipes();
    cleanup_shared_memory();
    
    printf("[Server] Server shutdown complete\n");
    return 0;
}
