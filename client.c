#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#define PORT 55000
#define BUFFER_SIZE 2048

static const char *CLR_RESET = "\033[0m";
static const char *CLR_CYAN = "\033[94m";    // calm blue for headings/status
static const char *CLR_YELLOW = "\033[93m";  // warm yellow for prompts
static const char *CLR_GREEN = "\033[91m";   // soft red for emphasis
static const char *CLR_MAGENTA = "\033[96m"; // soft cyan for contrast

static char g_you_name[64] = {0};
static char g_opponent_name[64] = {0};
static int g_opponent_letters = 0;
static int g_opponent_guesses = 0;

static size_t sanitize_name(const char *in, char *out, size_t out_sz) {
    if (!in || !out || out_sz == 0)
        return 0;
    size_t len = 0;
    while (*in == ' ')
        in++;
    for (; *in && len + 1 < out_sz; in++) {
        if (!isprint((unsigned char)*in))
            continue;
        out[len++] = *in;
    }
    while (len > 0 && out[len - 1] == ' ')
        len--;
    out[len] = '\0';
    return len;
}

static void spinner(const char *label, int steps) {
    static const char frames[] = "|/-\\";
    printf("%s%s%s ", CLR_CYAN, label, CLR_RESET);
    fflush(stdout);
    for (int i = 0; i < steps; i++) {
        printf("\r%s%s %c%s ", CLR_CYAN, label, frames[i % 4], CLR_RESET);
        fflush(stdout);
        usleep(120000);
    }
    printf("\r%s%s ...%s\n", CLR_CYAN, label, CLR_RESET);
}

static void intro_animation(void) {
    const char *lines[] = {
        "  ______                        ______                       _ ",
        " / _____)                      / _____)                     | |",
        "| /  ___ _   _  ____  ___  ___| /  ___ _   _  ____  ___  ___| |",
        "| | (___) | | |/ _  )/___)/___) | (___) | | |/ _  )/___)/___)_|",
        "| \\____/| |_| ( (/ /|___ |___ | \\____/| |_| ( (/ /|___ |___ |_ ",
        " \\_____/ \\____|\\____|___/(___/ \\_____/ \\____|\\____|___/(___/|_|"};
    size_t count = sizeof(lines) / sizeof(lines[0]);
    printf("\n%s", CLR_CYAN);
    for (size_t i = 0; i < count; i++) {
        for (const char *p = lines[i]; *p; p++) {
            putchar(*p);
            fflush(stdout);
            usleep(8000);
        }
        putchar('\n');
        fflush(stdout);
        usleep(40000);
    }
    printf("%s", CLR_RESET);
    printf("%sWELCOME TO GUESS-GUESS!%s  Guess letters, crack the word, have fun!\n\n",
           CLR_YELLOW, CLR_RESET);
}

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void send_line(int fd, const char *line) {
    size_t len = strlen(line);
    if (send(fd, line, len, 0) == -1) {
        die("send");
    }
}

static void send_json(int fd, const char *fmt, ...) {
    char buffer[BUFFER_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer) - 2, fmt, args);
    va_end(args);
    strncat(buffer, "\n", sizeof(buffer) - strlen(buffer) - 1);
    send_line(fd, buffer);
}

static int read_line(int fd, char *out, size_t out_sz) {
    size_t pos = 0;
    while (pos + 1 < out_sz) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) {
            return 0;
        } else if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (c == '\n') {
            break;
        }
        out[pos++] = c;
    }
    out[pos] = '\0';
    return (int)pos;
}

static int parse_int_field(const char *json, const char *key, int *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *loc = strstr(json, pattern);
    if (!loc)
        return 0;
    loc += strlen(pattern);
    *out = atoi(loc);
    return 1;
}

static int parse_string_field(const char *json, const char *key, char *out, size_t out_sz) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *loc = strstr(json, pattern);
    if (!loc)
        return 0;
    loc += strlen(pattern);
    const char *end = strchr(loc, '"');
    if (!end)
        return 0;
    size_t len = (size_t)(end - loc);
    if (len >= out_sz)
        len = out_sz - 1;
    strncpy(out, loc, len);
    out[len] = '\0';
    return 1;
}

static int prompt_yes_no(const char *prompt) {
    char line[16];
    while (1) {
        printf("%s (y/n): ", prompt);
        if (!fgets(line, sizeof(line), stdin)) {
            return 0;
        }
        if (line[0] == 'y' || line[0] == 'Y')
            return 1;
        if (line[0] == 'n' || line[0] == 'N')
            return 0;
        printf("Please enter y or n.\n");
    }
}

static void render_status_panel(const char *you_label, const char *opp_label, int letters_found,
                                int guesses_left, int opp_letters, int opp_guess,
                                const char *progress) {
    printf("%s--------------------------------------------%s\n", CLR_CYAN, CLR_RESET);
    if (progress && progress[0]) {
        printf(" Progress : %s\n", progress);
    }
    printf(" %-9s | Letters: %-3d | Word guesses left: %-2d\n", you_label, letters_found,
           guesses_left);
    printf(" %-9s | Letters: %-3d | Word guesses left: %-2d\n", opp_label, opp_letters,
           opp_guess);
    printf("--------------------------------------------\n");
}

static void flash_first_player(const char *p1, const char *p2, const char *winner) {
    const char *name1 = (p1 && p1[0]) ? p1 : "Player 1";
    const char *name2 = (p2 && p2[0]) ? p2 : "Player 2";
    const char *names[2] = {name1, name2};
    const char *final_winner = (winner && winner[0]) ? winner : name1;

    printf("%sChoosing who starts...%s\n", CLR_YELLOW, CLR_RESET);
    int flashes = 10;
    for (int i = 0; i < flashes; i++) {
        const char *pick = names[rand() % 2];
        printf("\r%s> %s%s", CLR_CYAN, pick, CLR_RESET);
        fflush(stdout);
        usleep(100000 + (i * 15000));
    }
    printf("\r%s> %s begins!%s\n", CLR_YELLOW, final_winner, CLR_RESET);
}

static void render_turn_ui(const char *you, const char *opponent, int letters_found,
                           int guesses_left, const char *progress, int opp_letters,
                           int opp_guess) {
    const char *you_label = (you && you[0]) ? you : "You";
    const char *opp_label = (opponent && opponent[0]) ? opponent : "Opponent";

    printf("\n%s[%s's turn]%s\n", CLR_CYAN, you_label, CLR_RESET);
    render_status_panel(you_label, opp_label, letters_found, guesses_left, opp_letters, opp_guess,
                        progress);
    printf("%sEnter one letter, then (optionally) try a full word/phrase guess.%s\n", CLR_YELLOW,
           CLR_RESET);
}

static void render_result_banner(const char *who, const char *result, char letter,
                                 const char *progress, int guesses_left) {
    int is_correct = strcmp(result, "correct") == 0;
    const char *clr = is_correct ? CLR_CYAN : CLR_GREEN; // correct: blue, incorrect: red
    const char *label = is_correct ? "CORRECT" : "INCORRECT";
    const char *who_label = (who && who[0]) ? who : "You";
    printf("\n%s%s: %s (letter: %c)%s\n", clr, who_label, label, letter ? letter : '?',
           CLR_RESET);
    if (progress && progress[0]) {
        printf("Progress        : %s\n", progress);
    }
    if (guesses_left >= 0) {
        printf("Word guesses left: %d\n", guesses_left);
    }
}

static void prompt_turn(int sock_fd, const char *you, const char *opponent) {
    char letter_input[BUFFER_SIZE];
    char word_input[BUFFER_SIZE];

    const char *you_label = (you && you[0]) ? you : "You";
    const char *opp_label = (opponent && opponent[0]) ? opponent : "Opponent";
    printf("%sYour move, %s!%s (racing against %s)\n", CLR_CYAN, you_label, CLR_RESET,
           opp_label);

    while (1) {
        printf("Guess a letter: ");
        fflush(stdout);
        
        if (!fgets(letter_input, sizeof(letter_input), stdin)) {
            exit(0);
        }
        if (strlen(letter_input) > 0 && letter_input[strlen(letter_input) - 1] == '\n') {
            letter_input[strlen(letter_input) - 1] = '\0';
        }
        if (strlen(letter_input) == 1 && isalpha(letter_input[0])) {
            break;
        }
        printf("Please enter a single alphabetic character.\n");
    }

    printf("Optional full word/phrase guess (press Enter to skip): ");
    fflush(stdout);
    if (!fgets(word_input, sizeof(word_input), stdin)) {
        exit(0);
    }
    if (strlen(word_input) > 0 && word_input[strlen(word_input) - 1] == '\n') {
        word_input[strlen(word_input) - 1] = '\0';
    }

    send_json(sock_fd,
              "{\"op\":\"TURN\",\"letter\":\"%c\",\"word\":\"%s\"}",
              tolower(letter_input[0]), word_input);
}

int main(int argc, char *argv[]) {
    srand((unsigned int)time(NULL));
    const char *host = "127.0.0.1";
    int port = PORT;
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = atoi(argv[2]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return 1;
        }
    }

    printf("Connecting to %s:%d ...\n", host, port);

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        die("socket");
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        die("inet_pton");
    }
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        die("connect");
    }

    char buffer[BUFFER_SIZE];

    // initial welcome
    intro_animation();
    printf("How to play:\n");
    printf("1) Enter a username.\n");
    printf("2) Choose to create a room or join an existing room.\n");
    printf("3) A coin flip decides who starts. On your turn, enter ONE letter and optionally a "
           "full-word guess.\n");
    printf("   - Letters are unlimited; word guesses: 3 per round.\n");
    printf("4) Turns alternate; first to reveal all letters or guess the word wins the race.\n");
    printf("5) After a round, both players can request a rematch and keep score.\n\n");
    if (read_line(sock_fd, buffer, sizeof(buffer)) > 0) {
        char msg[256] = {0};
        parse_string_field(buffer, "message", msg, sizeof(msg));
        if (msg[0]) {
            printf("%s\n", msg);
        }
    }

    char username[64];
    printf("Enter username: ");
    if (!fgets(username, sizeof(username), stdin)) {
        return 0;
    }
    if (strlen(username) > 0 && username[strlen(username) - 1] == '\n') {
        username[strlen(username) - 1] = '\0';
    }

    char clean_name[sizeof(g_you_name)] = {0};
    if (sanitize_name(username, clean_name, sizeof(clean_name)) == 0) {
        printf("Invalid/empty username. Exiting.\n");
        return 0;
    }

    strncpy(g_you_name, clean_name, sizeof(g_you_name) - 1);

    send_json(sock_fd, "{\"op\":\"HELLO\",\"user\":\"%s\"}", clean_name);

    printf("%sMain Menu:%s\n", CLR_YELLOW, CLR_RESET);
    printf("1) Create room\n");
    printf("2) Join room\n");
    printf("Select option (1/2): ");
    char choice[8];
    if (!fgets(choice, sizeof(choice), stdin)) {
        return 0;
    }
    int room_id = 0;
    if (choice[0] == '1') {
        send_json(sock_fd, "{\"op\":\"CREATE_ROOM\"}");
    } else {
        printf("Enter room id to join: ");
        char room_buf[32];
        if (!fgets(room_buf, sizeof(room_buf), stdin)) {
            return 0;
        }
        room_id = atoi(room_buf);
        send_json(sock_fd, "{\"op\":\"JOIN_ROOM\",\"room\":%d}", room_id);
    }

    while (1) {
        int n = read_line(sock_fd, buffer, sizeof(buffer));
        if (n == 0) {
            printf("Server closed connection.\n");
            break;
        } else if (n < 0) {
            perror("recv");
            break;
        }

        if (strstr(buffer, "\"type\":\"ERROR\"")) {
            char msg[256];
            if (parse_string_field(buffer, "message", msg, sizeof(msg))) {
                printf("Error: %s\n", msg);
            } else {
                printf("%s\n", buffer);
            }
            continue;
        }

        if (strstr(buffer, "\"type\":\"INFO\"") || strstr(buffer, "\"type\":\"WELCOME\"") ||
            strstr(buffer, "\"type\":\"ROOM_CREATED\"") || strstr(buffer, "\"type\":\"ROOM_JOINED\"") ||
            strstr(buffer, "\"type\":\"PONG\"")) {
            char msg[256] = {0};
            int room = 0;
            int seat = 0;
            parse_string_field(buffer, "message", msg, sizeof(msg));
            parse_int_field(buffer, "room", &room);
            parse_int_field(buffer, "seat", &seat);

            if (strstr(buffer, "\"type\":\"WELCOME\"")) {
                printf("%s\n", msg[0] ? msg : "Welcome!");
            } else if (strstr(buffer, "\"type\":\"ROOM_CREATED\"")) {
                printf("%sRoom created! Share this room id with your friend: %d%s\n",
                       CLR_YELLOW, room, CLR_RESET);
                spinner("Waiting for opponent to join", 10);
            } else if (strstr(buffer, "\"type\":\"ROOM_JOINED\"")) {
                printf("%sJoined room %d (seat %d). Waiting for start...%s\n", CLR_YELLOW, room,
                       seat, CLR_RESET);
                spinner("Waiting for game start", 10);
            } else if (msg[0]) {
                printf("%s\n", msg);
                if (strstr(msg, "Waiting for opponent")) {
                    spinner("Waiting", 10);
                }
            }
            continue;
        }

        if (strstr(buffer, "\"type\":\"REMATCH_DECLINED\"")) {
            char msg[256] = {0};
            parse_string_field(buffer, "message", msg, sizeof(msg));
            printf("\nOpponent declined rematch. %s\n", msg[0] ? msg : "");
            printf("Returning to lobby/quit.\n");
            break;
        }

        if (strstr(buffer, "\"type\":\"GAME_START\"")) {
            int len = 0;
            char category[128] = {0};
            char hint[256] = {0};
            char you_name[64] = {0};
            char opponent_name[64] = {0};
            char first_player[64] = {0};
            parse_int_field(buffer, "word_length", &len);
            parse_string_field(buffer, "category", category, sizeof(category));
            parse_string_field(buffer, "hint", hint, sizeof(hint));
            parse_string_field(buffer, "you", you_name, sizeof(you_name));
            parse_string_field(buffer, "opponent", opponent_name, sizeof(opponent_name));
            parse_string_field(buffer, "first_player", first_player, sizeof(first_player));

            if (you_name[0])
                strncpy(g_you_name, you_name, sizeof(g_you_name) - 1);
            if (opponent_name[0])
                strncpy(g_opponent_name, opponent_name, sizeof(g_opponent_name) - 1);
            g_opponent_letters = 0;
            g_opponent_guesses = 3;

            printf("\n=== GAME START ===\n");
            printf("Category : %s\n", category);
            printf("Hint     : %s\n", hint);
            printf("Word len : %d\n", len);
            if (g_you_name[0] || g_opponent_name[0]) {
                printf("Matchup  : %s vs %s\n",
                       g_you_name[0] ? g_you_name : "You",
                       g_opponent_name[0] ? g_opponent_name : "Opponent");
            }
            flash_first_player(g_you_name, g_opponent_name, first_player);
            printf("Word guesses: 3 (letters are unlimited, but word guess have 3 chances)\n\n");
            continue;
        }

        if (strstr(buffer, "\"type\":\"TURN\"")) {
            int guesses_left = 0;
            int letters_found = 0;
            int opp_letters = 0;
            int opp_guess = 0;
            int your_turn = 0;
            char progress[BUFFER_SIZE] = {0};
            char you_name[64] = {0};
            char opponent_name[64] = {0};
            parse_int_field(buffer, "guesses_left", &guesses_left);
            parse_int_field(buffer, "letters_found", &letters_found);
            parse_int_field(buffer, "opponent_letters", &opp_letters);
            parse_int_field(buffer, "opponent_guesses_left", &opp_guess);
            parse_int_field(buffer, "your_turn", &your_turn);
            parse_string_field(buffer, "progress", progress, sizeof(progress));
            parse_string_field(buffer, "player", you_name, sizeof(you_name));
            parse_string_field(buffer, "opponent", opponent_name, sizeof(opponent_name));

            if (you_name[0])
                strncpy(g_you_name, you_name, sizeof(g_you_name) - 1);
            if (opponent_name[0])
                strncpy(g_opponent_name, opponent_name, sizeof(g_opponent_name) - 1);
            g_opponent_letters = opp_letters;
            g_opponent_guesses = opp_guess;

            const char *you_label = g_you_name[0] ? g_you_name : "You";
            const char *opp_label = g_opponent_name[0] ? g_opponent_name : "Opponent";

            if (your_turn) {
                render_turn_ui(you_name, opponent_name, letters_found, guesses_left, progress,
                               opp_letters, opp_guess);
                prompt_turn(sock_fd, you_label, opp_label);
            } else {
                printf("\n%sWaiting for %s's turn...%s\n", CLR_CYAN, opp_label, CLR_RESET);
                render_status_panel(you_label, opp_label, letters_found, guesses_left,
                                    opp_letters, opp_guess, progress);
            }
            continue;
        }

        if (strstr(buffer, "\"type\":\"RESULT\"")) {
            char result[32];
            char progress[BUFFER_SIZE];
            int guesses_left = 0;
            int letters_found = 0;
            char letter = '?';
            parse_string_field(buffer, "result", result, sizeof(result));
            parse_string_field(buffer, "progress", progress, sizeof(progress));
            parse_int_field(buffer, "guesses_left", &guesses_left);
            parse_int_field(buffer, "letters_found", &letters_found);
            char letter_buf[8] = {0};
            parse_string_field(buffer, "letter", letter_buf, sizeof(letter_buf));
            if (letter_buf[0])
                letter = letter_buf[0];
            render_result_banner("You", result, letter, progress, guesses_left);

            // Display opponent progress once after collecting all updates
            const char *opp_label = g_opponent_name[0] ? g_opponent_name : "Opponent";
            printf("\n%s progress: %d letters found, %d word guesses left.\n", 
                   opp_label, g_opponent_letters, g_opponent_guesses);
            continue;
        }

        if (strstr(buffer, "\"type\":\"OPPONENT_UPDATE\"")) {
            int letters = 0, guesses = 0;
            parse_int_field(buffer, "opponent_letters", &letters);
            parse_int_field(buffer, "opponent_guesses_left", &guesses);
            // Only update stored values, don't print yet
            g_opponent_letters = letters;
            g_opponent_guesses = guesses;
            continue;
        }

        if (strstr(buffer, "\"type\":\"WORD_RESULT\"")) {
            char result[32];
            parse_string_field(buffer, "result", result, sizeof(result));
            printf("Word guess %s.\n", result);

            // Display opponent progress once after word guess
            const char *opp_label = g_opponent_name[0] ? g_opponent_name : "Opponent";
            printf("\n%s progress: %d letters found, %d word guesses left.\n", 
                   opp_label, g_opponent_letters, g_opponent_guesses);
            continue;
        }

        if (strstr(buffer, "\"type\":\"GAME_OVER\"")) {
            char status[16];
            char word[BUFFER_SIZE];
            int you = 0, opp = 0;
            int your_wins = 0, opp_wins = 0;
            parse_string_field(buffer, "status", status, sizeof(status));
            parse_string_field(buffer, "word", word, sizeof(word));
            parse_int_field(buffer, "your_letters", &you);
            parse_int_field(buffer, "opponent_letters", &opp);
            parse_int_field(buffer, "your_wins", &your_wins);
            parse_int_field(buffer, "opponent_wins", &opp_wins);
            int is_win = strcmp(status, "win") == 0;
            int is_lose = strcmp(status, "lose") == 0;
            const char *clr = is_win ? CLR_GREEN : (is_lose ? CLR_MAGENTA : CLR_YELLOW);
            const char *you_label = g_you_name[0] ? g_you_name : "You";
            const char *opp_label = g_opponent_name[0] ? g_opponent_name : "Opponent";

            printf("\n%s=== GAME OVER ===%s\n", CLR_CYAN, CLR_RESET);
            if (is_win) {
                printf("%s _       _                                _ \n", clr);
                printf("( )  _  ( ) _                            ( )\n");
                printf("| | ( ) | |(_)  ___    ___     __   _ __ | |\n");
                printf("| | | | | || |/' _ `\\/' _ `\\ /'__`\\( '__)| |\n");
                printf("| (_/ \\_) || || ( ) || ( ) |(  ___/| |   | |\n");
                printf("`\\___x___/'(_)(_) (_)(_) (_)`\\____)(_)   (_)\n");
                printf("                                         (_)%s\n", CLR_RESET);
            } else if (is_lose) {
                printf("%s _                         \n", clr);
                printf("( )                        \n");
                printf("| |       _     ___    __  \n");
                printf("| |  _  /'_`\\ /',__) /'__`\\\n");
                printf("| |_( )( (_) )\\__, \\(  ___/\n");
                printf("(____/'`\\___/'(____/'\\____)%s\n", CLR_RESET);
            } else {
                printf("Result   : %s%s%s\n", clr, status, CLR_RESET);
            }
            printf("Word     : %s\n", word);
            printf("Match    : %s vs %s\n", you_label, opp_label);
            printf("%s      : %d letters\n", you_label, you);
            printf("%s : %d letters\n", opp_label, opp);
            printf("Score    : %s %d - %d %s\n\n", you_label, your_wins, opp_wins, opp_label);

            if (prompt_yes_no("Play another round?")) {
                send_json(sock_fd, "{\"op\":\"REMATCH\"}");
                spinner("Waiting for opponent to agree", 12);
                continue;
            } else {
                send_json(sock_fd, "{\"op\":\"DECLINE_REMATCH\"}");
                printf("Thanks for playing!\n");
                break;
            }
        }

        // fallback
        printf("%s\n", buffer);
    }

    close(sock_fd);
    return 0;
}
