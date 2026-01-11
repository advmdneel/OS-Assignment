/*
 * Concurrent Networked Board Game Client
 * Race to 100 - A multiplayer dice racing game
 *
 * Communicates with server via named pipes (single-machine mode)
 */

#include "common.h"

// Pipe file descriptors
int pipe_write_fd = -1;
int pipe_read_fd = -1;
int player_slot = -1;

volatile sig_atomic_t client_running = 1;

void sigint_handler(int sig) {
    (void)sig;
    client_running = 0;
    printf("\n[Client] Shutting down...\n");
}

void print_help(void) {
    printf("\n=== COMMANDS ===\n");
    printf("  roll    - Roll the dice (when it's your turn)\n");
    printf("  status  - View current game state and scores\n");
    printf("  help    - Show this help message\n");
    printf("  quit    - Leave the game\n");
    printf("================\n\n");
}

void print_game_rules(void) {
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║              🎲 RACE TO 100 - GAME RULES 🎲              ║\n");
    printf("╠═══════════════════════════════════════════════════════════╣\n");
    printf("║  OBJECTIVE: Be the first player to reach 100 points!      ║\n");
    printf("║                                                           ║\n");
    printf("║  HOW TO PLAY:                                             ║\n");
    printf("║  • On your turn, type 'roll' to roll two dice             ║\n");
    printf("║  • The sum of both dice is added to your score            ║\n");
    printf("║  • First player to reach 100 or more WINS!                ║\n");
    printf("║                                                           ║\n");
    printf("║  SPECIAL RULE - SNAKE EYES:                               ║\n");
    printf("║  • If you roll double 1s (snake eyes), your entire        ║\n");
    printf("║    score resets to ZERO! Be careful!                      ║\n");
    printf("║                                                           ║\n");
    printf("║  Players: 3-5 | Turn Order: Round Robin                   ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");
}

int connect_to_server(int slot) {
    char pipe_to_server[64], pipe_to_client[64];
    
    snprintf(pipe_to_server, sizeof(pipe_to_server), "%s%d_to_server", PIPE_BASE, slot);
    snprintf(pipe_to_client, sizeof(pipe_to_client), "%s%d_to_client", PIPE_BASE, slot);
    
    printf("[Client] Connecting to server on slot %d...\n", slot + 1);
    
    // Open write pipe to server
    pipe_write_fd = open(pipe_to_server, O_WRONLY);
    if (pipe_write_fd < 0) {
        perror("Failed to connect to server (write pipe)");
        return -1;
    }
    
    // Open read pipe from server
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
    ssize_t bytes = read(pipe_read_fd, msg, sizeof(GameMessage));
    return (bytes > 0) ? 0 : -1;
}

void handle_response(GameMessage *response) {
    switch (response->type) {
        case MSG_PLAYER_JOINED:
            printf("\n✓ %s\n", response->text);
            break;
            
        case MSG_YOUR_TURN:
            printf("\n>>> YOUR TURN! Type 'roll' to roll the dice.\n");
            break;
            
        case MSG_ROLL_RESULT:
            printf("\n🎲 %s\n", response->text);
            break;
            
        case MSG_WAIT:
            printf("\n⏳ %s\n", response->text);
            break;
            
        case MSG_GAME_STATE:
            printf("%s", response->text);
            break;
            
        case MSG_GAME_OVER:
            printf("\n");
            printf("╔════════════════════════════════════════╗\n");
            printf("║           🏆 GAME OVER! 🏆            ║\n");
            printf("╠════════════════════════════════════════╣\n");
            printf("║  %s\n", response->text);
            printf("╚════════════════════════════════════════╝\n");
            break;
            
        case MSG_PLAYER_LEFT:
            printf("\n%s\n", response->text);
            break;
            
        case MSG_GAME_START:
            printf("\n🎮 GAME STARTED! 🎮\n");
            printf("%s\n", response->text);
            break;
            
        case MSG_ERROR:
            printf("\n❌ Error: %s\n", response->text);
            break;
            
        default:
            if (strlen(response->text) > 0) {
                printf("\n%s\n", response->text);
            }
            break;
    }
}

int main(int argc, char *argv[]) {
    int slot = 0;
    char player_name[MAX_NAME_LEN] = "";
    
    // Parse command line arguments
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
    
    strncpy(player_name, argv[2], MAX_NAME_LEN - 1);
    player_name[MAX_NAME_LEN - 1] = '\0';
    
    // Setup signal handler
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    
    print_game_rules();
    
    // Connect to server
    if (connect_to_server(slot) < 0) {
        printf("Make sure the server is running!\n");
        return 1;
    }
    
    player_slot = slot;
    
    // Send join message
    GameMessage msg, response;
    memset(&msg, 0, sizeof(GameMessage));
    msg.type = MSG_JOIN;
    msg.player_id = slot;
    strncpy(msg.player_name, player_name, MAX_NAME_LEN - 1);
    send_message(&msg);
    
    // Get join response
    if (receive_message(&response) == 0) {
        handle_response(&response);
    }
    
    print_help();
    
    // Main game loop
    char input[64];
    
    while (client_running) {
        printf("\n[%s]> ", player_name);
        fflush(stdout);
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        
        // Remove newline
        input[strcspn(input, "\n")] = '\0';
        
        // Skip empty input
        if (strlen(input) == 0) {
            continue;
        }
        
        memset(&msg, 0, sizeof(GameMessage));
        msg.player_id = slot;
        
        if (strcmp(input, "roll") == 0 || strcmp(input, "r") == 0) {
            msg.type = MSG_ROLL;
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
    
    // Cleanup
    close(pipe_read_fd);
    close(pipe_write_fd);
    
    printf("[Client] Goodbye!\n");
    return 0;
}
