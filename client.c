/*
 * Concurrent Networked Sudoku Game Client
 * Collaborative Sudoku - Players compete to solve a shared puzzle
 *
 * Minix/Mini OS Compatible Version
 * Single-file version - no external headers needed
 * 
 * Compile: gcc -o client client.c
 * Run: ./client <slot 0-4> <player_name>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

// ============================================================================
// Configuration & Constants (from common.h)
// ============================================================================

#define MAX_PLAYERS 5
#define MAX_NAME_LEN 32
#define MAX_LOG_MSG 256
#define GRID_SIZE 9
#define EMPTY_CELL 0
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

typedef struct {
    int id;
    char name[MAX_NAME_LEN];
    int score;
    int correct_placements;
    int wrong_placements;
    PlayerState state;
    int handler_pid;
} Player;

typedef struct {
    int value;
    int solution;
    int is_fixed;
    int placed_by;
} SudokuCell;

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
// Global Variables
// ============================================================================

int pipe_write_fd = -1;
int pipe_read_fd = -1;
int player_slot = -1;
char my_name[MAX_NAME_LEN] = "";

SudokuCell local_grid[GRID_SIZE][GRID_SIZE];
Player local_players[MAX_PLAYERS];
int local_cells_remaining = 0;
int local_num_players = 0;
int local_current_turn = -1;

volatile sig_atomic_t client_running = 1;

// ============================================================================
// Signal Handler
// ============================================================================

void sigint_handler(int sig) {
    (void)sig;
    client_running = 0;
    printf("\n[Client] Shutting down...\n");
}

// ============================================================================
// Display Functions
// ============================================================================

void print_help(void) {
    printf("\n=== COMMANDS ===\n");
    printf("  place R C N  - Place number N at row R, column C\n");
    printf("               - Example: 'place 3 5 7' puts 7 at row 3, col 5\n");
    printf("  p R C N      - Short form of place\n");
    printf("  status       - View current game state and scores\n");
    printf("  grid         - Display the Sudoku grid\n");
    printf("  help         - Show this help message\n");
    printf("  quit         - Leave the game\n");
    printf("================\n\n");
}

void print_game_rules(void) {
    printf("\n");
    printf("+-----------------------------------------------------------+\n");
    printf("|           COLLABORATIVE SUDOKU - GAME RULES               |\n");
    printf("+-----------------------------------------------------------+\n");
    printf("|  OBJECTIVE: Work to solve the Sudoku puzzle and earn      |\n");
    printf("|             the most points!                              |\n");
    printf("|                                                           |\n");
    printf("|  HOW TO PLAY:                                             |\n");
    printf("|  * On your turn, place a number (1-9) in an empty cell    |\n");
    printf("|  * Use: place <row> <col> <number>                        |\n");
    printf("|  * Example: 'place 3 5 7' puts 7 at row 3, column 5       |\n");
    printf("|                                                           |\n");
    printf("|  SCORING:                                                 |\n");
    printf("|  * Correct placement: +10 points                          |\n");
    printf("|  * Wrong placement:   -5 points (cell stays empty)        |\n");
    printf("|                                                           |\n");
    printf("|  WINNING:                                                 |\n");
    printf("|  * Game ends when puzzle is complete                      |\n");
    printf("|  * Player with the highest score wins!                    |\n");
    printf("|                                                           |\n");
    printf("|  GRID LEGEND:                                             |\n");
    printf("|  * [X] = Fixed number (given in puzzle)                   |\n");
    printf("|  * (X) = Placed by a player                               |\n");
    printf("|  *  .  = Empty cell                                       |\n");
    printf("|                                                           |\n");
    printf("|  Players: 3-5 | Turn Order: Round Robin                   |\n");
    printf("+-----------------------------------------------------------+\n\n");
}

void print_grid(void) {
    printf("\n");
    printf("    +-------+-------+-------+\n");
    printf("      1 2 3   4 5 6   7 8 9\n");
    printf("    +-------+-------+-------+\n");
    
    for (int r = 0; r < GRID_SIZE; r++) {
        if (r > 0 && r % 3 == 0) {
            printf("    +-------+-------+-------+\n");
        }
        
        printf(" %d  |", r + 1);
        
        for (int c = 0; c < GRID_SIZE; c++) {
            if (c > 0 && c % 3 == 0) {
                printf("|");
            }
            
            SudokuCell *cell = &local_grid[r][c];
            
            if (cell->value == EMPTY_CELL) {
                printf(" .");
            } else if (cell->is_fixed) {
                printf(" %d", cell->value);
            } else {
                if (cell->placed_by == player_slot) {
                    printf("*%d", cell->value);
                } else {
                    printf("+%d", cell->value);
                }
            }
        }
        printf("|\n");
    }
    
    printf("    +-------+-------+-------+\n");
    printf("\n  Legend: N=fixed  *N=yours  +N=other player\n");
    printf("  Cells remaining: %d\n\n", local_cells_remaining);
}

void print_scoreboard(void) {
    printf("\n=== SCOREBOARD ===\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (local_players[i].state != PLAYER_DISCONNECTED) {
            printf("  Player %d: %-12s | Score: %4d | Correct: %2d | Wrong: %2d %s\n",
                   i + 1,
                   local_players[i].name,
                   local_players[i].score,
                   local_players[i].correct_placements,
                   local_players[i].wrong_placements,
                   (local_current_turn == i) ? " <-- TURN" : "");
        }
    }
    printf("==================\n");
}

// ============================================================================
// Network Functions
// ============================================================================

int connect_to_server(int slot) {
    char pipe_to_server[64], pipe_to_client[64];
    
    snprintf(pipe_to_server, sizeof(pipe_to_server), "%s%d_to_server", PIPE_BASE, slot);
    snprintf(pipe_to_client, sizeof(pipe_to_client), "%s%d_to_client", PIPE_BASE, slot);
    
    printf("[Client] Connecting to server on slot %d...\n", slot + 1);
    
    pipe_write_fd = open(pipe_to_server, O_WRONLY);
    if (pipe_write_fd < 0) {
        perror("Failed to connect to server (write pipe)");
        return -1;
    }
    
    pipe_read_fd = open(pipe_to_client, O_RDONLY);
    if (pipe_read_fd < 0) {
        perror("Failed to connect to server (read pipe)");
        close(pipe_write_fd);
        return -1;
    }
    
    printf("[Client] Connected!\n");
    return 0;
}

void send_message(GameMessage *msg) {
    write(pipe_write_fd, msg, sizeof(GameMessage));
}

int receive_message(GameMessage *msg) {
    // Blocking read from the server -> client pipe.
    // We call this only when select() says data is ready.
    ssize_t bytes = read(pipe_read_fd, msg, sizeof(GameMessage));
    return (bytes > 0) ? 0 : -1;
}

void update_local_state(GameMessage *msg) {
    // Keep a local copy so the client can print without asking the server again.
    memcpy(local_grid, msg->grid, sizeof(local_grid));
    memcpy(local_players, msg->players, sizeof(local_players));
    local_cells_remaining = msg->cells_remaining;
    local_num_players = msg->num_players;
    local_current_turn = msg->current_turn;
}

// ============================================================================
// Response Handler
// ============================================================================

void handle_response(GameMessage *response) {
    // IMPORTANT:
    // Most server messages include a fresh copy of the game state.
    // Updating local state first ensures the grid/scoreboard prints correctly.
    if (response->type == MSG_PLAYER_JOINED || 
        response->type == MSG_GAME_START ||
        response->type == MSG_PLACE_RESULT ||
        response->type == MSG_GAME_STATE ||
        response->type == MSG_GRID_UPDATE ||
        response->type == MSG_GAME_OVER ||
        response->type == MSG_WAIT) {
        update_local_state(response);
    }
    
    switch (response->type) {
        case MSG_PLAYER_JOINED:
            printf("\n[OK] %s\n", response->text);
            break;
            
        case MSG_GAME_START:
            printf("\n");
            printf("+========================================+\n");
            printf("|        *** GAME STARTED! ***           |\n");
            printf("+========================================+\n");
            printf("  %s\n", response->text);
            printf("+========================================+\n");
            print_grid();
            print_scoreboard();
            if (local_current_turn == player_slot) {
                printf("\n>>> IT'S YOUR TURN! Use 'place R C N' to place a number.\n");
            }
            break;
            
        case MSG_YOUR_TURN:
            // Server tells THIS client it is our turn (personal message).
            printf("\n");
            printf("+========================================+\n");
            if (strlen(response->text) > 0) {
                printf("  %s\n", response->text);
            } else {
                printf("  >>> IT'S YOUR TURN! Use 'place R C N' to place a number.\n");
            }
            printf("+========================================+\n");
            print_grid();
            print_scoreboard();
            break;
            
        case MSG_PLACE_RESULT:
            if (response->success) {
                printf("\n[+] %s\n", response->text);
            } else {
                printf("\n[-] %s\n", response->text);
            }
            print_grid();
            print_scoreboard();
            break;
            
        case MSG_WAIT:
            // "Wait" can mean "not your turn" OR just a turn notification.
            printf("\n[WAIT] %s\n", response->text);
            // Also update the grid to show the latest state if game is in progress
            if (local_cells_remaining > 0) {
                print_grid();
                print_scoreboard();
            }
            break;
            
        case MSG_GRID_UPDATE:
            // Another player played -> refresh our view automatically.
            printf("\n[UPDATE] %s\n", response->text);
            print_grid();
            print_scoreboard();
            if (local_current_turn == player_slot) {
                printf("\n>>> IT'S YOUR TURN! Use 'place R C N' to place a number.\n");
            }
            break;
            
        case MSG_GAME_STATE:
            printf("\n%s\n", response->text);
            print_grid();
            print_scoreboard();
            break;
            
        case MSG_GAME_OVER:
            printf("\n");
            printf("+========================================+\n");
            printf("|       *** PUZZLE COMPLETE! ***         |\n");
            printf("+========================================+\n");
            printf("  %s\n", response->text);
            printf("+========================================+\n");
            print_grid();
            print_scoreboard();
            break;
            
        case MSG_PLAYER_LEFT:
            printf("\n%s\n", response->text);
            break;
            
        case MSG_ERROR:
            printf("\n[ERROR] %s\n", response->text);
            break;
            
        default:
            if (strlen(response->text) > 0) {
                printf("\n%s\n", response->text);
            }
            break;
    }
}

// ============================================================================
// Command Parser
// ============================================================================

int parse_place_command(const char *input, int *row, int *col, int *value) {
    char cmd[16];
    int r, c, v;
    
    if (sscanf(input, "%15s %d %d %d", cmd, &r, &c, &v) == 4) {
        if (strcmp(cmd, "place") == 0 || strcmp(cmd, "p") == 0) {
            *row = r - 1;
            *col = c - 1;
            *value = v;
            return 1;
        }
    }
    
    return 0;
}

// ============================================================================
// Main Function
// ============================================================================

int main(int argc, char *argv[]) {
    int slot = 0;
    
    if (argc < 3) {
        printf("Usage: %s <slot 0-%d> <player_name>\n", argv[0], MAX_PLAYERS - 1);
        printf("Example: %s 0 Alice\n", argv[0]);
        return 1;
    }
    
    slot = atoi(argv[1]);
    if (slot < 0 || slot >= MAX_PLAYERS) {
        printf("Invalid slot. Must be 0-%d\n", MAX_PLAYERS - 1);
        return 1;
    }
    
    strncpy(my_name, argv[2], MAX_NAME_LEN - 1);
    my_name[MAX_NAME_LEN - 1] = '\0';
    
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    
    print_game_rules();
    
    if (connect_to_server(slot) < 0) {
        printf("Make sure the server is running!\n");
        return 1;
    }
    
    player_slot = slot;
    
    memset(local_grid, 0, sizeof(local_grid));
    memset(local_players, 0, sizeof(local_players));
    
    GameMessage msg, response;
    memset(&msg, 0, sizeof(GameMessage));
    msg.type = MSG_JOIN;
    msg.player_id = slot;
    strncpy(msg.player_name, my_name, MAX_NAME_LEN - 1);
    send_message(&msg);
    
    if (receive_message(&response) == 0) {
        handle_response(&response);
    }
    
    print_help();
    
    char input[64];
    fd_set read_fds;
    struct timeval timeout;
    int max_fd = (pipe_read_fd > STDIN_FILENO) ? pipe_read_fd : STDIN_FILENO;
    max_fd++;
    
    while (client_running) {
        // Use select() so the client can:
        // - read server updates immediately
        // - still accept keyboard input
        // If we only used fgets(), the client would block and NOT auto-update.
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(pipe_read_fd, &read_fds);
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms timeout
        
        int ready = select(max_fd, &read_fds, NULL, NULL, &timeout);
        
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        // Check for incoming messages from server
        if (FD_ISSET(pipe_read_fd, &read_fds)) {
            if (receive_message(&response) == 0) {
                handle_response(&response);
            }
        }
        
        // Check for user input
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            const char *turn_indicator = "";
            if (local_current_turn == player_slot) {
                turn_indicator = " [YOUR TURN]";
            }
            
            printf("\n[%s%s]> ", my_name, turn_indicator);
            fflush(stdout);
            
            if (fgets(input, sizeof(input), stdin) == NULL) {
                break;
            }
            
            input[strcspn(input, "\n")] = '\0';
            
            if (strlen(input) == 0) {
                continue;
            }
            
            memset(&msg, 0, sizeof(GameMessage));
            msg.player_id = slot;
            
            int row, col, value;
            
            if (parse_place_command(input, &row, &col, &value)) {
                if (row < 0 || row >= GRID_SIZE || col < 0 || col >= GRID_SIZE) {
                    printf("[ERROR] Row and column must be 1-9\n");
                    continue;
                }
                if (value < 1 || value > 9) {
                    printf("[ERROR] Number must be 1-9\n");
                    continue;
                }
                
                msg.type = MSG_PLACE;
                msg.row = row;
                msg.col = col;
                msg.value = value;
                send_message(&msg);
                
                if (receive_message(&response) == 0) {
                    handle_response(&response);
                }
            }
            else if (strcmp(input, "status") == 0 || strcmp(input, "s") == 0) {
                msg.type = MSG_GAME_STATE;
                send_message(&msg);
                
                if (receive_message(&response) == 0) {
                    handle_response(&response);
                }
            }
            else if (strcmp(input, "grid") == 0 || strcmp(input, "g") == 0) {
                print_grid();
                print_scoreboard();
            }
            else if (strcmp(input, "help") == 0 || strcmp(input, "h") == 0) {
                print_help();
            }
            else if (strcmp(input, "quit") == 0 || strcmp(input, "q") == 0) {
                msg.type = MSG_QUIT;
                send_message(&msg);
                
                if (receive_message(&response) == 0) {
                    handle_response(&response);
                }
                break;
            }
            else {
                printf("Unknown command. Type 'help' for available commands.\n");
            }
        }
    }
    
    close(pipe_read_fd);
    close(pipe_write_fd);
    
    printf("[Client] Goodbye!\n");
    return 0;
}
