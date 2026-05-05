#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PORT 55000
#define BACKLOG 10
#define MAX_CLIENTS 100
#define MAX_ROOMS 50
#define MAX_WORD_LEN 128
#define MAX_NAME_LEN 32
#define BUFFER_SIZE 2048

typedef struct Player {
    int socket_fd;
    char username[MAX_NAME_LEN];
    int room_id;                 // -1 if not in a room
    int word_guesses_left;       // remaining full-word attempts
    int letters_found;           // total letters revealed for this player
    char progress[MAX_WORD_LEN]; // masked word for this player
    char guessed_letters[64];    // track duplicates
    int wins;                    // rounds won while connected
} Player;

typedef struct {
    int id; // 1-based
    int active;
    int started;
    int finished;
    char word[MAX_WORD_LEN];
    char category[64];
    char hint[128];
    int total_letters; // non-space characters
    Player *players[2];
    int rematch_ready[2];
    time_t rematch_ready_ts[2];
    int rematch_timer_active;
    int current_turn; // index of player whose turn it is (0 or 1), -1 if unset
} Room;

static Room rooms[MAX_ROOMS];
static pthread_mutex_t rooms_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const char *word;
    const char *category;
    const char *hint;
} WordEntry;

static void send_json(int fd, const char *fmt, ...);
static Room *get_room_by_id(int room_id);

static const WordEntry words[] = {
    // Pinoy foods
    {"Taho", "Pinoy Foods", "Sweet tofu snack with syrup and sago"},
    {"Sisig", "Pinoy Foods", "Crispy chopped pork dish"},
    {"Adobo", "Pinoy Foods", "Chicken or pork stewed in soy and vinegar"},
    {"Lechon", "Pinoy Foods", "Roasted whole pig"},
    {"Sinigang", "Pinoy Foods", "Sour soup with meat and vegetables"},
    {"Puto", "Pinoy Foods", "Steamed rice cake"},
    {"Afritada", "Pinoy Foods", "Tomato-based chicken or pork stew"},
    {"Turon", "Pinoy Foods", "Fried banana roll"},
    {"Lumpia", "Pinoy Foods", "Filipino spring rolls"},
    {"Menudo", "Pinoy Foods", "Pork and liver stew with vegetables"},

    // Tourist spots
    {"Bohol", "Tourist Spots in the PH", "Famous for Chocolate Hills"},
    {"Cebu", "Tourist Spots in the PH", "Historical sites and beaches"},
    {"Boracay", "Tourist Spots in the PH", "White sand beaches"},
    {"Palawan", "Tourist Spots in the PH", "Stunning islands and lagoons"},
    {"Legazpi", "Tourist Spots in the PH", "Near Mayon Volcano"},
    {"Manila", "Tourist Spots in the PH", "Capital city of the Philippines"},
    {"Batanes", "Tourist Spots in the PH", "Picturesque northern islands"},
    {"Baguio", "Tourist Spots in the PH", "Cool mountain city"},
    {"Siargao", "Tourist Spots in the PH", "Surfing destination"},

    // Personalities
    {"Anne Curtis", "Personalities in the PH", "Actress and TV host"},
    {"Vice Ganda", "Personalities in the PH", "Comedian and TV host"},
    {"Vic Sotto", "Personalities in the PH", "Actor and comedian"},
    {"Coco Martin", "Personalities in the PH", "Actor known for Ang Probinsyano"},
    {"Kim Chiu", "Personalities in the PH", "Actress and singer"},
    {"Kathryn Bernardo", "Personalities in the PH", "Actress in TV and movies"},
    {"Nadine Lustre", "Personalities in the PH", "Actress and singer"},

    // Local games
    {"Trumpo", "Local Games", "Traditional spinning top game"},
    {"Patintero", "Local Games", "Tag game on lines drawn on the ground"},
    {"Chinese Garter", "Local Games", "Jumping over stretched garter"},
    {"Sungka", "Local Games", "Shell-and-board game"},
    {"Aragawang Base", "Local Games", "Tag game with bases"},

    // Pinoy brands
    {"Jollibee", "Pinoy Brands", "Popular fast-food chain"},
    {"Mang Inasal", "Pinoy Brands", "Filipino grilled chicken chain"},
    {"Globe", "Pinoy Brands", "Telecommunications company"},
    {"Smart", "Pinoy Brands", "Mobile network provider"},
    {"Goldilocks", "Pinoy Brands", "Bakery and cake shop"},
    {"Cebu Pacific", "Pinoy Brands", "Budget airline"},
    {"Penshoppe", "Pinoy Brands", "Clothing brand"},
    {"Bench", "Pinoy Brands", "Fashion retail brand"},
};

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static size_t sanitize_username(const char *in, char *out, size_t out_sz) {
    if (!in || !out || out_sz == 0)
        return 0;
    size_t len = 0;
    // skip leading spaces
    while (*in == ' ')
        in++;
    for (; *in && len + 1 < out_sz; in++) {
        if (!isprint((unsigned char)*in))
            continue;
        out[len++] = *in;
    }
    // trim trailing spaces
    while (len > 0 && out[len - 1] == ' ')
        len--;
    out[len] = '\0';
    return len;
}

typedef struct {
    int room_id;
    int timer_seconds;
} RematchTimerArgs;

static void *rematch_timer_thread(void *arg) {
    RematchTimerArgs *targs = (RematchTimerArgs *)arg;
    int room_id = targs->room_id;
    int seconds = targs->timer_seconds;
    free(targs);

    sleep((unsigned int)seconds);

    pthread_mutex_lock(&rooms_mutex);
    Room *room = get_room_by_id(room_id);
    if (room && room->finished && room->rematch_timer_active) {
        int ready0 = room->rematch_ready[0];
        int ready1 = room->rematch_ready[1];
        if (!(ready0 && ready1)) {
            Player *p0 = room->players[0];
            Player *p1 = room->players[1];
            if (p0) {
                send_json(p0->socket_fd,
                          "{\"type\":\"REMATCH_DECLINED\",\"message\":\"Rematch timed out\"}");
            }
            if (p1) {
                send_json(p1->socket_fd,
                          "{\"type\":\"REMATCH_DECLINED\",\"message\":\"Rematch timed out\"}");
            }
            room->rematch_ready[0] = room->rematch_ready[1] = 0;
        }
        room->rematch_timer_active = 0;
    }
    pthread_mutex_unlock(&rooms_mutex);
    return NULL;
}

static void schedule_rematch_timeout(Room *room, int seconds) {
    if (!room || room->rematch_timer_active)
        return;
    room->rematch_timer_active = 1;
    RematchTimerArgs *args = malloc(sizeof(RematchTimerArgs));
    if (!args) {
        room->rematch_timer_active = 0;
        return;
    }
    args->room_id = room->id;
    args->timer_seconds = seconds;
    pthread_t tid;
    if (pthread_create(&tid, NULL, rematch_timer_thread, args) != 0) {
        room->rematch_timer_active = 0;
        free(args);
        return;
    }
    pthread_detach(tid);
}

static void send_line(int fd, const char *line) {
    size_t len = strlen(line);
    if (send(fd, line, len, 0) == -1) {
        perror("send");
    }
}

// newline-delimited JSON framing
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
            return 0; // disconnect
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

static int parse_string_field(const char *json, const char *key, char *out, size_t out_sz) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *loc = strstr(json, pattern);
    if (!loc) {
        return 0;
    }
    loc += strlen(pattern);
    const char *end = strchr(loc, '"');
    if (!end) {
        return 0;
    }
    size_t len = (size_t)(end - loc);
    if (len >= out_sz) {
        len = out_sz - 1;
    }
    strncpy(out, loc, len);
    out[len] = '\0';
    return 1;
}

static int parse_int_field(const char *json, const char *key, int *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *loc = strstr(json, pattern);
    if (!loc) {
        return 0;
    }
    loc += strlen(pattern);
    *out = atoi(loc);
    return 1;
}

static const WordEntry *choose_word(void) {
    size_t count = sizeof(words) / sizeof(words[0]);
    return &words[rand() % count];
}

static void mask_word(const char *word, char *out) {
    size_t len = strlen(word);
    for (size_t i = 0; i < len; i++) {
        if (word[i] == ' ') {
            out[i] = ' ';
        } else {
            out[i] = '_';
        }
    }
    out[len] = '\0';
}

static int count_letters(const char *word) {
    int total = 0;
    for (size_t i = 0; word[i]; i++) {
        if (word[i] != ' ')
            total++;
    }
    return total;
}

static int get_player_index(Room *room, Player *player) {
    if (!room)
        return -1;
    if (room->players[0] == player)
        return 0;
    if (room->players[1] == player)
        return 1;
    return -1;
}

static void send_state(Room *room) {
    if (!room || !room->started)
        return;
    Player *p1 = room->players[0];
    Player *p2 = room->players[1];
    if (!p1 || !p2)
        return;

    const char *p1_name = strlen(p1->username) ? p1->username : "Player 1";
    const char *p2_name = strlen(p2->username) ? p2->username : "Player 2";
    int turn = (room->current_turn == 0 || room->current_turn == 1) ? room->current_turn : 0;

    send_json(p1->socket_fd,
              "{\"type\":\"TURN\",\"your_turn\":%d,\"guesses_left\":%d,"
              "\"letters_found\":%d,\"progress\":\"%s\","
              "\"opponent_letters\":%d,\"opponent_guesses_left\":%d,"
              "\"player\":\"%s\",\"opponent\":\"%s\"}",
              turn == 0 ? 1 : 0, p1->word_guesses_left, p1->letters_found, p1->progress,
              p2->letters_found, p2->word_guesses_left, p1_name, p2_name);

    send_json(p2->socket_fd,
              "{\"type\":\"TURN\",\"your_turn\":%d,\"guesses_left\":%d,"
              "\"letters_found\":%d,\"progress\":\"%s\","
              "\"opponent_letters\":%d,\"opponent_guesses_left\":%d,"
              "\"player\":\"%s\",\"opponent\":\"%s\"}",
              turn == 1 ? 1 : 0, p2->word_guesses_left, p2->letters_found, p2->progress,
              p1->letters_found, p1->word_guesses_left, p2_name, p1_name);
}

static void end_game(Room *room, int winner_index) {
    if (!room || room->finished)
        return;
    room->finished = 1;
    room->started = 0;
    room->current_turn = -1;
    const char *status_p1 = "draw";
    const char *status_p2 = "draw";
    if (winner_index == 0) {
        status_p1 = "win";
        status_p2 = "lose";
        if (room->players[0])
            room->players[0]->wins++;
    } else if (winner_index == 1) {
        status_p1 = "lose";
        status_p2 = "win";
        if (room->players[1])
            room->players[1]->wins++;
    }

    Player *p1 = room->players[0];
    Player *p2 = room->players[1];
    if (p1) {
        send_json(p1->socket_fd,
                  "{\"type\":\"GAME_OVER\",\"status\":\"%s\",\"word\":\"%s\","
                  "\"your_letters\":%d,\"opponent_letters\":%d,"
                  "\"your_wins\":%d,\"opponent_wins\":%d}",
                  status_p1, room->word, p1->letters_found, p2 ? p2->letters_found : 0,
                  p1->wins, p2 ? p2->wins : 0);
    }
    if (p2) {
        send_json(p2->socket_fd,
                  "{\"type\":\"GAME_OVER\",\"status\":\"%s\",\"word\":\"%s\","
                  "\"your_letters\":%d,\"opponent_letters\":%d,"
                  "\"your_wins\":%d,\"opponent_wins\":%d}",
                  status_p2, room->word, p2->letters_found, p1 ? p1->letters_found : 0,
                  p2->wins, p1 ? p1->wins : 0);
    }
}

static void reset_room(Room *room) {
    if (!room)
        return;
    memset(room, 0, sizeof(*room));
}

static Room *get_room_by_id(int room_id) {
    if (room_id <= 0 || room_id > MAX_ROOMS)
        return NULL;
    Room *room = &rooms[room_id - 1];
    return room->active ? room : NULL;
}

static int ensure_room_space(Player *player, int room_id, int *slot_out) {
    Room *room = get_room_by_id(room_id);
    if (!room || room->finished)
        return 0;
    if (room->players[0] && room->players[1])
        return 0;
    *slot_out = room->players[0] ? 1 : 0;
    room->players[*slot_out] = player;
    player->room_id = room->id;
    return 1;
}

static Room *create_room(Player *player) {
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (!rooms[i].active) {
            rooms[i].active = 1;
            rooms[i].id = i + 1;
            rooms[i].started = 0;
            rooms[i].finished = 0;
            rooms[i].players[0] = player;
            rooms[i].players[1] = NULL;
            rooms[i].current_turn = -1;
            player->room_id = rooms[i].id;
            return &rooms[i];
        }
    }
    return NULL;
}

static void start_game(Room *room) {
    room->started = 1;
    room->finished = 0;
    room->rematch_timer_active = 0;
    room->current_turn = (rand() % 2); // random deciding factor for first player
    const WordEntry *entry = choose_word();
    strncpy(room->word, entry->word, sizeof(room->word) - 1);
    room->word[sizeof(room->word) - 1] = '\0';
    strncpy(room->category, entry->category, sizeof(room->category) - 1);
    room->category[sizeof(room->category) - 1] = '\0';
    strncpy(room->hint, entry->hint, sizeof(room->hint) - 1);
    room->hint[sizeof(room->hint) - 1] = '\0';
    room->total_letters = count_letters(room->word);

    for (int i = 0; i < 2; i++) {
        Player *p = room->players[i];
        Player *opp = room->players[1 - i];
        if (!p)
            continue;
        p->word_guesses_left = 3; // full-word attempts
        p->letters_found = 0;
        memset(p->guessed_letters, 0, sizeof(p->guessed_letters));
        mask_word(room->word, p->progress);
        const char *you_name = strlen(p->username) ? p->username : "You";
        const char *opp_name = (opp && strlen(opp->username)) ? opp->username : "Opponent";
        const char *first_name = (room->players[room->current_turn] &&
                                  strlen(room->players[room->current_turn]->username))
                                     ? room->players[room->current_turn]->username
                                     : (room->current_turn == 0 ? "Player 1" : "Player 2");
        send_json(p->socket_fd,
                  "{\"type\":\"GAME_START\",\"room\":%d,\"word_length\":%zu,"
                  "\"word_guesses\":3,\"category\":\"%s\",\"hint\":\"%s\","
                  "\"you\":\"%s\",\"opponent\":\"%s\",\"first_player\":\"%s\"}",
                  room->id, strlen(room->word), room->category, room->hint, you_name,
                  opp_name, first_name);
    }
    room->rematch_ready[0] = room->rematch_ready[1] = 0;
    send_state(room);
}

static int already_guessed(Player *p, char letter) {
    for (size_t i = 0; p->guessed_letters[i]; i++) {
        if (p->guessed_letters[i] == letter)
            return 1;
    }
    return 0;
}

static void add_guessed_letter(Player *p, char letter) {
    size_t len = strlen(p->guessed_letters);
    if (len + 1 < sizeof(p->guessed_letters)) {
        p->guessed_letters[len] = letter;
        p->guessed_letters[len + 1] = '\0';
    }
}

static int process_letter_guess(Room *room, Player *player, char letter) {
    int added = 0;
    for (size_t i = 0; room->word[i]; i++) {
        char w = tolower(room->word[i]);
        if (room->word[i] == ' ')
            continue;
        if (tolower(letter) == w && player->progress[i] == '_') {
            player->progress[i] = room->word[i];
            player->letters_found++;
            added = 1;
        }
    }
    return added;
}

static void handle_disconnect(Player *player) {
    pthread_mutex_lock(&rooms_mutex);
    Room *room = get_room_by_id(player->room_id);
    if (room) {
        for (int i = 0; i < 2; i++) {
            if (room->players[i] == player) {
                room->players[i] = NULL;
            }
        }
        if (!room->finished && room->started) {
            int survivor = room->players[0] ? 0 : (room->players[1] ? 1 : -1);
            if (survivor >= 0) {
                end_game(room, survivor);
            }
        }
        // If a rematch was pending, let the survivor know it won't start.
        if (room->finished && (room->rematch_ready[0] || room->rematch_ready[1])) {
            int survivor = room->players[0] ? 0 : (room->players[1] ? 1 : -1);
            if (survivor >= 0 && room->players[survivor]) {
                send_json(room->players[survivor]->socket_fd,
                          "{\"type\":\"REMATCH_DECLINED\",\"message\":\"Opponent left before "
                          "rematch\"}");
            }
            room->rematch_ready[0] = room->rematch_ready[1] = 0;
            room->rematch_timer_active = 0;
        }
        if (!room->players[0] && !room->players[1]) {
            reset_room(room);
        }
    }
    pthread_mutex_unlock(&rooms_mutex);
    close(player->socket_fd);
    free(player);
}

static void *client_thread(void *arg) {
    Player *player = (Player *)arg;
    char buffer[BUFFER_SIZE];

    send_json(player->socket_fd,
              "{\"type\":\"WELCOME\",\"message\":\"Welcome to GUESS-GUESS!\"}");
    while (1) {
        int n = read_line(player->socket_fd, buffer, sizeof(buffer));
        if (n == 0) {
            printf("Client disconnected\n");
            break;
        } else if (n < 0) {
            perror("recv");
            break;
        }

        if (strlen(buffer) >= BUFFER_SIZE - 1) {
            send_json(player->socket_fd,
                      "{\"type\":\"ERROR\",\"message\":\"Frame too large\"}");
            continue;
        }

        if (strstr(buffer, "\"op\":\"PING\"")) {
            send_json(player->socket_fd, "{\"type\":\"PONG\"}");
            continue;
        }

        if (strstr(buffer, "\"op\":\"HELLO\"")) {
            char name[MAX_NAME_LEN];
            if (!parse_string_field(buffer, "user", name, sizeof(name))) {
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Missing username\"}");
                continue;
            }
            char clean[MAX_NAME_LEN];
            size_t nlen = sanitize_username(name, clean, sizeof(clean));
            if (nlen == 0) {
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Invalid username\"}");
                continue;
            }
            strncpy(player->username, clean, sizeof(player->username) - 1);
            send_json(player->socket_fd,
                      "{\"type\":\"INFO\",\"message\":\"hello %s\"}", player->username);
            continue;
        }

        if (strstr(buffer, "\"op\":\"CREATE_ROOM\"")) {
            if (player->room_id != -1) {
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Already in a room\"}");
                continue;
            }
            pthread_mutex_lock(&rooms_mutex);
            Room *room = create_room(player);
            pthread_mutex_unlock(&rooms_mutex);
            if (!room) {
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"No available rooms\"}");
                continue;
            }
            send_json(player->socket_fd,
                      "{\"type\":\"ROOM_CREATED\",\"room\":%d,"
                      "\"message\":\"Share room id to start\"}",
                      room->id);
            continue;
        }

        if (strstr(buffer, "\"op\":\"JOIN_ROOM\"")) {
            int room_id = -1;
            if (!parse_int_field(buffer, "room", &room_id)) {
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Missing room id\"}");
                continue;
            }
            pthread_mutex_lock(&rooms_mutex);
            int slot = -1;
            int ok = ensure_room_space(player, room_id, &slot);
            Room *room = get_room_by_id(room_id);
            pthread_mutex_unlock(&rooms_mutex);
            if (!ok || !room) {
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Cannot join room\"}");
                continue;
            }
            send_json(player->socket_fd,
                      "{\"type\":\"ROOM_JOINED\",\"room\":%d,\"seat\":%d}", room->id,
                      slot + 1);
            if (room->players[0] && room->players[1] && !room->started) {
                pthread_mutex_lock(&rooms_mutex);
                start_game(room);
                pthread_mutex_unlock(&rooms_mutex);
            } else {
                send_json(player->socket_fd,
                          "{\"type\":\"INFO\",\"message\":\"Waiting for opponent\"}");
            }
            continue;
        }

        if (strstr(buffer, "\"op\":\"REMATCH\"")) {
            pthread_mutex_lock(&rooms_mutex);
            Room *room = get_room_by_id(player->room_id);
            if (!room || !room->finished) {
                pthread_mutex_unlock(&rooms_mutex);
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"No finished game to rematch\"}");
                continue;
            }
            int idx = get_player_index(room, player);
            if (idx < 0) {
                pthread_mutex_unlock(&rooms_mutex);
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Not seated in a room\"}");
                continue;
            }
            room->rematch_ready[idx] = 1;
            room->rematch_ready_ts[idx] = time(NULL);
            send_json(player->socket_fd,
                      "{\"type\":\"INFO\",\"message\":\"Rematch requested. Waiting for opponent\"}");
            Player *opponent = room->players[1 - idx];
            if (opponent) {
                send_json(opponent->socket_fd,
                          "{\"type\":\"INFO\",\"message\":\"Opponent wants a rematch\"}");
            }

            if (room->players[0] && room->players[1] && room->rematch_ready[0] &&
                room->rematch_ready[1]) {
                room->finished = 0;
                start_game(room);
            } else {
                schedule_rematch_timeout(room, 25);
            }
            pthread_mutex_unlock(&rooms_mutex);
            continue;
        }

        if (strstr(buffer, "\"op\":\"DECLINE_REMATCH\"")) {
            pthread_mutex_lock(&rooms_mutex);
            Room *room = get_room_by_id(player->room_id);
            if (!room || !room->finished) {
                pthread_mutex_unlock(&rooms_mutex);
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"No finished game to decline\"}");
                continue;
            }
            int idx = get_player_index(room, player);
            if (idx < 0) {
                pthread_mutex_unlock(&rooms_mutex);
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Not seated in a room\"}");
                continue;
            }
            room->rematch_ready[0] = room->rematch_ready[1] = 0;
            room->rematch_timer_active = 0;
            Player *opponent = room->players[1 - idx];
            if (opponent) {
                send_json(opponent->socket_fd,
                          "{\"type\":\"REMATCH_DECLINED\",\"message\":\"Opponent declined "
                          "the rematch\"}");
            }
            pthread_mutex_unlock(&rooms_mutex);
            send_json(player->socket_fd,
                      "{\"type\":\"INFO\",\"message\":\"Rematch declined\"}");
            continue;
        }

        if (strstr(buffer, "\"op\":\"TURN\"")) {
            pthread_mutex_lock(&rooms_mutex);
            Room *room = get_room_by_id(player->room_id);
            if (!room || !room->started || room->finished) {
                pthread_mutex_unlock(&rooms_mutex);
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"No active game\"}");
                continue;
            }

            int idx = get_player_index(room, player);
            if (idx < 0) {
                pthread_mutex_unlock(&rooms_mutex);
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Not seated in this room\"}");
                continue;
            }
            if (room->current_turn != idx) {
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Not your turn\"}");
                send_state(room);
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }
            char letter_str[8] = {0};
            char word_guess[MAX_WORD_LEN] = {0};
            parse_string_field(buffer, "letter", letter_str, sizeof(letter_str));
            parse_string_field(buffer, "word", word_guess, sizeof(word_guess));
            if (strlen(letter_str) != 1 || !isalpha(letter_str[0])) {
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Provide single letter\"}");
                if (!room->finished) {
                    send_state(room); // re-prompt the current turn
                }
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }
            char letter = (char)tolower(letter_str[0]);
            int winner = -1;

            if (word_guess[0] != '\0') {
                if (player->word_guesses_left <= 0) {
                    send_json(player->socket_fd,
                              "{\"type\":\"ERROR\",\"message\":\"No word guesses left\"}");
                    word_guess[0] = '\0'; // treat as skipped, continue with letter
                } else {
                    player->word_guesses_left--;
                    if (strcasecmp(word_guess, room->word) == 0) {
                        winner = idx;
                        strncpy(player->progress, room->word, sizeof(player->progress) - 1);
                        player->letters_found = room->total_letters;
                    } else {
                        send_json(player->socket_fd,
                                  "{\"type\":\"WORD_RESULT\",\"result\":\"incorrect\","
                                  "\"word\":\"%s\",\"guesses_left\":%d}",
                                  word_guess, player->word_guesses_left);
                        // Send opponent updates after incorrect word guess
                        Player *opponent = room->players[1 - idx];
                        if (opponent) {
                            // Send opponent's progress to the player who just guessed
                            send_json(player->socket_fd,
                                      "{\"type\":\"OPPONENT_UPDATE\",\"opponent_letters\":%d,"
                                      "\"opponent_guesses_left\":%d}",
                                      opponent->letters_found, opponent->word_guesses_left);
                            // Send player's progress to the opponent
                            send_json(opponent->socket_fd,
                                      "{\"type\":\"OPPONENT_UPDATE\",\"opponent_letters\":%d,"
                                      "\"opponent_guesses_left\":%d}",
                                      player->letters_found, player->word_guesses_left);
                        }
                    }
                }
            }

            // If the word was guessed correctly, skip letter handling and finish.
            if (winner != -1) {
                end_game(room, winner);
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }

            if (already_guessed(player, letter)) {
                send_json(player->socket_fd,
                          "{\"type\":\"ERROR\",\"message\":\"Letter already guessed\"}");
                if (!room->finished) {
                    send_state(room); // keep the same player's turn with an updated prompt
                }
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }
            add_guessed_letter(player, letter);

            int correct = process_letter_guess(room, player, letter);

            if (player->letters_found >= room->total_letters) {
                winner = idx;
            }

            send_json(player->socket_fd,
                      "{\"type\":\"RESULT\",\"result\":\"%s\",\"letter\":\"%c\","
                      "\"progress\":\"%s\",\"letters_found\":%d,\"guesses_left\":%d}",
                      correct ? "correct" : "incorrect", letter, player->progress,
                      player->letters_found, player->word_guesses_left);

            Player *opponent = room->players[1 - idx];
            if (opponent) {
                // Send opponent's progress to the player who just guessed
                send_json(player->socket_fd,
                          "{\"type\":\"OPPONENT_UPDATE\",\"opponent_letters\":%d,"
                          "\"opponent_guesses_left\":%d}",
                          opponent->letters_found, opponent->word_guesses_left);
                // Send player's progress to the opponent
                send_json(opponent->socket_fd,
                          "{\"type\":\"OPPONENT_UPDATE\",\"opponent_letters\":%d,"
                          "\"opponent_guesses_left\":%d}",
                          player->letters_found, player->word_guesses_left);
            }

            if (winner != -1) {
                end_game(room, winner);
                pthread_mutex_unlock(&rooms_mutex);
                continue;
            }

            // Move to the other player's turn
            room->current_turn = (room->current_turn == 0) ? 1 : 0;
            send_state(room);
            pthread_mutex_unlock(&rooms_mutex);
            continue;
        }

        send_json(player->socket_fd,
                  "{\"type\":\"ERROR\",\"message\":\"Unknown operation\"}");
    }

    handle_disconnect(player);
    return NULL;
}

int main(void) {
    srand((unsigned int)time(NULL));
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        die("socket");
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        die("setsockopt");
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        die("bind");
    }

    if (listen(server_fd, BACKLOG) == -1) {
        die("listen");
    }

    printf("Server listening on port %d\n", PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1) {
            perror("accept");
            continue;
        }
        printf("Client connected from %s:%d\n", inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        Player *player = calloc(1, sizeof(Player));
        if (!player) {
            fprintf(stderr, "Failed to allocate player\n");
            close(client_fd);
            continue;
        }
        player->socket_fd = client_fd;
        player->room_id = -1;
        player->wins = 0;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, player) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(player);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
