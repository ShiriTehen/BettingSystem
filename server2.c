/**
 * 
 * 
 * */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>
#define KEEP_ALIVE_INTERVAL 5
#define KEEP_ALIVE_TIMEOUT 10
#define TEAM_NAME_MAX_LENGTH 100 
#define PORT 8080
#define MULTICAST_PORT 8081
#define MULTICAST_GROUP "239.0.0.1"
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 10
#define GAME_DURATION 10  // Duration until the game starts
//#define GAME_LENGTH 20 // Length of the game in seconds
#define HalfTimer_server_respose 10 // Time for clien.

#define NUM_COUNTRIES (sizeof(countries) / sizeof(countries[0]))

pthread_mutex_t lock;
time_t start_time;
typedef enum {
    CLIENT_STATE_AUTHENTICATION,     // Waiting for client to authenticate
    CLIENT_STATE_BET,                // Waiting for client to place a bet
    CLIENT_STATE_READY,              // Ready to receive updates
    CLIENT_STATE_AFTER_HALF          // After halftime, checking for doubling the bet
} ClientState;

typedef struct {
    int TCP_socket;
    int UDP_socket;
    struct sockaddr_in address;
    int client_id;
    int bet_team;
    int bet_amount;
    int bet_received;
    int recive_halftime;  // Flag to indicate if halftime response received
    char comments[BUFFER_SIZE];
    int connected;
    ClientState state;
    time_t last_keep_alive;  // Time of the last keep-alive response
    int missed_keep_alives;  // Number of missed keep-alive responses
    
} Client;
typedef struct {
    char message;
    int seq_num;
} Packet;
typedef struct {
    int score[2];
    int current_minute;
    int game_running;
    int halftime;
    char group1[BUFFER_SIZE]; 
    char group2[BUFFER_SIZE]; 
    time_t last_keep_alive;  // Time of the last keep-alive response
    int missed_keep_alives;  // Number of missed keep-alive responses
} GameState;

Client *clients[MAX_CLIENTS];
GameState game_state;
int client_count = 0;
int udp_multicast_socket;
struct sockaddr_in multicast_addr;
int DebugMode=0;
int sequence_number = 0;


const char* countries[] = {
    "Brazil", "Germany", "Argentina", "France", "Spain", 
    "Italy", "England", "Netherlands", "Portugal", "Belgium"
};

//tests
int test_drop_halftime=0                ;// A test for halftime message dropping. DONE
int test_multicast_to_wrong_reciver =1;     // A test for wrong address client message .DONE
int test_delay_keep_alive_ack=0;
int GAME_LENGTH =20;
int HalfTimer_client_respose =5;
/**TO-DO
 *  test_drop_final_message=0;//TO-DO
 * int test_drop_password=0;//TO-DO
 *  test_drop_place_bet=0;//TO-DO
 * 
 */
void accept_bets(int server_fd);
void setup_udp_multicast();
void handle_signal(int signal);
void notify_clients_of_interruption();
void close_all_client_sockets();
void broadcast_game_update(const char *message);
void broadcast_half_time_message();
void log_client_message(Client *client, const char *message);  
void format_game_update(char* update, size_t buffer_size, const GameState* game_state) ;
void assign_teams(GameState* game_state);
void start_game();
void* simulate_game(void* arg);
void* handle_client_requests(void* arg);
void* handle_new_client(void* arg);
void send_final_message(const Client *client, int wrong_message);
void shuffle_countries(char* shuffled_countries[], int n);
void* keep_alive_manager(void* arg) ;

int main() {
    srand(time(NULL)); // Seed for random number generator
    GameState game_state;


    signal(SIGINT, handle_signal);  // Handle Ctrl+C
    signal(SIGTSTP, handle_signal); // Handle Ctrl+Z
 


    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    pthread_mutex_init(&lock, NULL);

    // TCP socket creation
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    printf("TCP socket created successfully.\n");

    // Set socket options
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("Socket options set successfully.\n");

    // Bind the TCP socket
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("Socket bound successfully.\n");

    // Setup UDP multicast
    setup_udp_multicast();
    printf("UDP multicast setup completed.\n");

    // Start listening for incoming TCP connections
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("Server is listening on port %d.\n", PORT);
    pthread_t keep_alive_thread;
    pthread_create(&keep_alive_thread, NULL, keep_alive_manager, NULL);
    accept_bets(server_fd);
    close_all_client_sockets();
    pthread_exit(NULL);
    return 0;
}

void* simulate_game(void* arg) {
    pthread_mutex_lock(&lock);
    game_state.current_minute = 0;
    game_state.score[0] = 0;
    game_state.score[1] = 0;
    game_state.game_running = 1;
    pthread_mutex_unlock(&lock);


    printf("THE GAME HAS STARTED!\n");

    for (int j = 1; j <= GAME_LENGTH; j++) {
        pthread_mutex_lock(&lock);
        game_state.current_minute = j;
        int team_that_made_goal = rand() % 2;
        int goal = rand() % 2;
        game_state.score[team_that_made_goal] += goal;
        pthread_mutex_unlock(&lock);

        char update[BUFFER_SIZE];
        format_game_update(update, BUFFER_SIZE, &game_state);
        broadcast_game_update(update);
        //printf("%s", update);  // Print to server's console
        
        // Check for halftime
        if (j == GAME_LENGTH / 2) {
            broadcast_half_time_message();
            sleep(HalfTimer_server_respose);  // Wait for clients to respond

            // Check for missing responses
            pthread_mutex_lock(&lock);
            for (int i = 0; i < client_count; i++) {
                if (!clients[i]->recive_halftime) {
                    printf("Client %d did not respond to halftime, assuming 'NO'.\n", clients[i]->client_id);
                    clients[i]->recive_halftime = 1; // Assume 'NO' if no response
                    clients[i]->state = CLIENT_STATE_AFTER_HALF;
                    
                }
            }
            pthread_mutex_unlock(&lock);
        }

        sleep(1);
    }
    pthread_mutex_lock(&lock);
    game_state.game_running = 0;
    pthread_mutex_unlock(&lock);

    // Delay to ensure the final messages are received by clients
    sleep(2);

    for (int i = 0; i < client_count; i++) {
        Client *client = clients[i];
        send_final_message(client, test_multicast_to_wrong_reciver);
        //sleep(1);
    }

    client_count = 0;

    // Exit the server after all client sockets are closed and the final messages are sent
    close_all_client_sockets();
     exit(0);
}

void* handle_client_requests(void* arg) {
    Client *client = (Client *)arg;
    char buffer[BUFFER_SIZE];
    while (1) {
        int bytes_received = recv(client->TCP_socket, buffer, BUFFER_SIZE, 0);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            printf("Received '%s' from client %d\n", buffer, client->client_id);
            if (strstr(buffer, "CLIENT_TERMINATED")) {
                printf("Client %d has terminated the connection. Decreasing client count.\n", client->client_id);
                pthread_mutex_lock(&lock);
                client_count--;
                pthread_mutex_unlock(&lock);
                close(client->TCP_socket);
                close(client->UDP_socket);
                client->connected = 0;
                return NULL;
            } 
            else if (strstr(buffer, "REQUEST_HALFTIME_MESSAGE")) 
            {
                // Resend the halftime message if requested
                pthread_mutex_lock(&lock);
                char half_time_message[BUFFER_SIZE];
                snprintf(half_time_message, BUFFER_SIZE, "HALFTIME\n");
                send(client->TCP_socket, half_time_message, strlen(half_time_message), 0);
                pthread_mutex_unlock(&lock);
            } 
                        
            else if (strstr(buffer, "YES") || strstr(buffer, "NO")) {
                pthread_mutex_lock(&lock);  // Lock the mutex to ensure thread safety

                if (strstr(buffer, "YES")) {
                    if (client->state != CLIENT_STATE_AFTER_HALF && client->recive_halftime == 0) {
                        client->bet_amount *= 2;  // Double the bet
                        printf("Client %d chose to double their bet.\n", client->client_id);
                    } 
                    else if (client->state == CLIENT_STATE_AFTER_HALF) {
                        const char *message = "TOOLATE";
                        send(client->TCP_socket, message, strlen(message), 0);  // Send "TOOLATE" message to client
                        printf("Client %d tried to double their bet too late.\n", client->client_id);
                    } else {
                        printf("Unexpected condition for client %d.\n", client->client_id);
                    }
                } 
                else if (strstr(buffer, "NO")) {
                    printf("Client %d chose not to double their bet.\n", client->client_id);
                }

                client->recive_halftime = 1;  // Mark that the client has responded
                client->state = CLIENT_STATE_AFTER_HALF;  // Ensure the state is set to AFTER_HALF

                pthread_mutex_unlock(&lock);  // Unlock the mutex after the critical section
                //printf("Client %d chose to %s their bet.\n", client->client_id, strstr(buffer, "YES") ? "double" : "not double");
            }
          
            else if (strncmp(buffer, "REQUEST_FINAL_MESSAGE", 21) == 0) {
                printf("Client %d requested the correct final message.\n", client->client_id);
                send_final_message(client, 0); }// Send the correct final message
            
            else if (strstr(buffer, "KEEP_ALIVE_ACK")) {
                pthread_mutex_lock(&lock);
                client->last_keep_alive = time(NULL);  // Update last keep-alive response time
                client->missed_keep_alives = 0;  // Reset missed keep-alive count
                pthread_mutex_unlock(&lock);
                printf("Received keep-alive ACK from client %d.\n", client->client_id);
            }




            else {
                printf("[THIS MESSEGE ISNT CLASSIFIED BY THE SERVER!!!!!!!! ]client : %d sent : %s\n", client->client_id, buffer);
            }
        } 
        else if (bytes_received == 0) {
            // Connection closed by client
            close(client->TCP_socket);
            printf("Client %d disconnected. Socket closed.\n", client->client_id);
            break;
        } 
        else
        {
            // Error occurred
            perror("recv");
            close(client->TCP_socket);
            printf("Client %d disconnected due to error. Socket closed.\n", client->client_id);
            break;
        }
    }

    return NULL;
}

void* handle_new_client(void* arg) {
    Client *client = (Client *)arg;
    char buffer[BUFFER_SIZE];

    // Check if the game is already running
    pthread_mutex_lock(&lock);
    if (game_state.game_running == 1) {
        const char *message = "ALREADYSTARTED";
        send(client->TCP_socket, message, strlen(message), 0);
        close(client->TCP_socket);
        printf("Client %d attempted to connect after the game started. Disconnecting.\n", client->client_id);
        client->connected = 0;
        free(client);
        client_count--;
        pthread_mutex_unlock(&lock);
        pthread_exit(NULL);
    }
    pthread_mutex_unlock(&lock);

    int remaining_time = GAME_DURATION - difftime(time(NULL), start_time);
    snprintf(buffer, BUFFER_SIZE, "Welcome %.100s vs %.100s %d %d %d %d", 
             game_state.group1, game_state.group2, remaining_time, GAME_LENGTH, HalfTimer_client_respose,KEEP_ALIVE_TIMEOUT);
    printf("Unicast to %d: %s\n", client->client_id, buffer);

    if (!test_drop_password) {
        send(client->TCP_socket, buffer, strlen(buffer), 0);
    } else {
        printf("Simulating dropped password prompt for client %d.\n", client->client_id);
    }

    // Main loop to handle client requests
    while (1) {
        struct timeval tv;
        fd_set readfds;
        remaining_time = GAME_DURATION - difftime(time(NULL), start_time);

        // Use short timeouts to allow other threads (e.g., broadcast) to run
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        FD_ZERO(&readfds);
        FD_SET(client->TCP_socket, &readfds);

        int result = select(client->TCP_socket + 1, &readfds, NULL, NULL, &tv);
        if (result > 0) {
            int bytes_read = recv(client->TCP_socket, buffer, BUFFER_SIZE, 0);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                printf("Received message from client %d: %s\n", client->client_id, buffer);
                if (strncmp(buffer, "AUTH:", 5) == 0) {
                    char* received_password = buffer + 5; // Skip the "AUTH:" prefix
                    char *expected_password = "1234";  // Expected password
                    pthread_mutex_lock(&lock);
                    if (game_state.game_running == 1) {
                        const char *message = "ALREADYSTARTED";
                        send(client->TCP_socket, message, strlen(message), 0);
                        close(client->TCP_socket);
                        close(client->UDP_socket);
                        client->connected = 0;
                        free(client);
                        client_count--;
                        pthread_mutex_unlock(&lock);
                        pthread_exit(NULL);
                    }

                    if (strcmp(received_password, expected_password) == 0) {
                        snprintf(buffer, BUFFER_SIZE, "Password accepted");
                        printf("Client %d sent correct password.\n", client->client_id);
                        send(client->TCP_socket, buffer, strlen(buffer), 0);
                        client->state = CLIENT_STATE_AUTHENTICATION;
                    } else {
                        snprintf(buffer, BUFFER_SIZE, "Incorrect password");
                        send(client->TCP_socket, buffer, strlen(buffer), 0);
                        close(client->TCP_socket);
                        printf("Client %d sent an incorrect password. Disconnecting.\n", client->client_id);
                        client->connected = 0;
                        free(client);
                        client_count--;
                        pthread_mutex_unlock(&lock);
                        pthread_exit(NULL);
                    }
                    pthread_mutex_unlock(&lock);
                } 
                else if (strncmp(buffer, "BET:", 4) == 0) {
                    int bet_team, bet_amount;
                    sscanf(buffer + 4, "%d %d", &bet_team, &bet_amount);
                    client->bet_team = bet_team;
                    client->bet_amount = bet_amount;
                    client->bet_received = 1;
                    client->connected =1;
                    printf("Client %d placed a bet on team %d with amount %d.\n", 
                            client->client_id, client->bet_team, client->bet_amount);
                    pthread_t request_thread;
                    pthread_create(&request_thread, NULL, handle_client_requests, (void *)client);
                    pthread_detach(request_thread);
                    break;
                }
            } else if (bytes_read == 0) {
                // Client disconnected
                close(client->TCP_socket);
                close(client->UDP_socket);
                printf("The game started.\n Client %d didnt finish instelize. closing its socket .\n", client->client_id);
                client->connected = 0;
                free(client);
                client_count--;
                pthread_exit(NULL);
            } else {
                perror("recv");
                close(client->TCP_socket);
                close(client->UDP_socket);
                client->connected = 0;
                free(client);
                client_count--;
                pthread_exit(NULL);
            }
        }

        // Update remaining time
        remaining_time = GAME_DURATION - difftime(time(NULL), start_time);
        if (remaining_time <= 0) {
            printf("Client %d did not authenticate/bet in time. Disconnecting.\n", client->client_id);
            close(client->TCP_socket);
            client->connected = 0;
            free(client);
            client_count--;
            pthread_exit(NULL);
        }
    }
    return NULL;
}

void* broadcast_remaining_time(void* arg) {
    char buffer[BUFFER_SIZE];

    while (1) {
        int remaining_time;

        // Only lock when accessing the shared resource
        pthread_mutex_lock(&lock);
        remaining_time = GAME_DURATION - difftime(time(NULL), start_time);
        pthread_mutex_unlock(&lock);

        if (remaining_time <= 0) {
            remaining_time = 0;
        }

        snprintf(buffer, BUFFER_SIZE, "COUNTDOWN %d\n", remaining_time);
        broadcast_game_update(buffer);

        if (remaining_time <= 0 && !game_state.game_running) {
            start_game();
            break;
        }

        sleep(1); // Broadcast every second
    }

    return NULL;
}

void start_game() {
    pthread_t game_thread;
    pthread_create(&game_thread, NULL, simulate_game, NULL);
}

void accept_bets(int server_fd) {
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int new_socket;

    start_time = time(NULL);

    // Assign teams before clients connect
    assign_teams(&game_state);  // Assign team names before any client joins

    pthread_t broadcast_thread;
    pthread_create(&broadcast_thread, NULL, broadcast_remaining_time, NULL);

    while (1) {
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);

        if (new_socket >= 0) {
            pthread_mutex_lock(&lock);
            if (game_state.game_running) {
                const char *message = "ALREADYSTARTED";
                send(new_socket, message, strlen(message), 0);
                close(new_socket);
                printf("Client attempted to join after game started. Socket closed.\n");
                pthread_mutex_unlock(&lock);
                continue;
            }
            Client *client = (Client *)malloc(sizeof(Client));
            client->TCP_socket = new_socket;
            client->address = address;
            client->client_id = client_count++;
            client->bet_received = 0;
            client->recive_halftime = 0;
            memset(client->comments, 0, BUFFER_SIZE);
            printf("Client %d logged in\n", client->client_id);
            clients[client_count - 1] = client;
            pthread_mutex_unlock(&lock);
            pthread_t client_thread;
            pthread_create(&client_thread, NULL, handle_new_client, (void *)client);
            pthread_detach(client_thread);
        }
    }

    pthread_join(broadcast_thread, NULL);
}

void setup_udp_multicast() {
    // Create a UDP socket
    if ((udp_multicast_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("UDP socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Initialize the multicast group address
    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_addr.s_addr = inet_addr(MULTICAST_GROUP);
    multicast_addr.sin_port = htons(MULTICAST_PORT);
}

void broadcast_game_update(const char *message) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, "%d:%s", sequence_number++, message);

    // Send the message to the multicast group
    if (sendto(udp_multicast_socket, buffer, strlen(buffer), 0, 
               (struct sockaddr*)&multicast_addr, sizeof(multicast_addr)) < 0) {
        perror("UDP multicast sendto failed");
    } else {
        printf("Broadcast: %s", buffer);
    }
}

void log_client_message(Client *client, const char *message) {
    snprintf(client->comments, BUFFER_SIZE, "Client %d sent: %s", client->client_id, message);
    printf("%s\n", client->comments);
}

void handle_signal(int signal) {
    if (signal == SIGINT || signal == SIGTSTP) {  // Handle Ctrl+C or Ctrl+Z
        printf("\nThe game has been interrupted by the server.\n");

        // Notify all clients that the game has been interrupted
        notify_clients_of_interruption();

        // Close all client sockets and exit
        close_all_client_sockets();
        close(udp_multicast_socket); // Close the UDP socket
        exit(0); // Exit the program
    }
}

void notify_clients_of_interruption() {
    char buffer[BUFFER_SIZE];-
    snprintf(buffer, BUFFER_SIZE, "The game has been interrupted by the server.\n");
    for (int i = 0; i < client_count; i++) {
        send(clients[i]->TCP_socket, buffer, strlen(buffer), 0);
        close(clients[i]->TCP_socket);
        close(clients[i]->UDP_socket);

    }
    printf("All clients have been notified of the interruption.\n");
}

void close_all_client_sockets() {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < client_count; i++) {
        if (clients[i]->connected) {
            close(clients[i]->TCP_socket);
            close(clients[i]->UDP_socket);
            printf("Closed socket for client %d.\n", clients[i]->client_id);
        }
        free(clients[i]);  // Free the memory allocated for each client
    }
    client_count = 0;  // Reset client count
    pthread_mutex_unlock(&lock);
}

void broadcast_half_time_message() {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, "HALFTIME");

    

    pthread_mutex_lock(&lock);
    for (int i = 0; i < client_count; i++) {
        if (clients[i]->connected == 0){printf("the server think that the client %d isnt connected",clients[i]->client_id);}

        if (clients[i]->connected) {
            if(test_drop_halftime){
                printf("Simulating dropped message for client %d.\n", clients[i]->client_id);
                continue; // Skip sending the message                
            }
                send(clients[i]->TCP_socket, buffer, strlen(buffer), 0); // Send the half-time message via TCP
                printf("Sent halftime message to client %d.\n", clients[i]->client_id);
        }
    }
    pthread_mutex_unlock(&lock);

    printf("Broadcasted halftime message to all clients.\n");

    // Set the halftime flag in GameState
    pthread_mutex_lock(&lock);
    game_state.halftime = 1;
    pthread_mutex_unlock(&lock);
}

void shuffle_countries(char* shuffled_countries[], int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        char* temp = shuffled_countries[i];
        shuffled_countries[i] = shuffled_countries[j];
        shuffled_countries[j] = temp;
    }
}

void assign_teams(GameState* game_state) {
    char* shuffled_countries[NUM_COUNTRIES];
    for (int i = 0; i < NUM_COUNTRIES; i++) {
        shuffled_countries[i] = strdup(countries[i]);
    }
    shuffle_countries(shuffled_countries, NUM_COUNTRIES);

    strncpy(game_state->group1, shuffled_countries[0], TEAM_NAME_MAX_LENGTH - 1);
    strncpy(game_state->group2, shuffled_countries[1], TEAM_NAME_MAX_LENGTH - 1);

    game_state->group1[TEAM_NAME_MAX_LENGTH - 1] = '\0';  // Ensure null-termination
    game_state->group2[TEAM_NAME_MAX_LENGTH - 1] = '\0';  // Ensure null-termination

    for (int i = 0; i < NUM_COUNTRIES; i++) {
        free(shuffled_countries[i]);
    }
}

void format_game_update(char* update, size_t buffer_size, const GameState* game_state) {
    int result = snprintf(update, buffer_size, 
                          "GAME UPDATE %d %d %d\n",
                          game_state->current_minute,
                           game_state->score[0],
                           game_state->score[1]);

    // Check if the output was truncated
    if (result >= buffer_size) {
        // Handle the truncation, for example by indicating the message was cut off
        snprintf(update, buffer_size, "Update message too long, some data was truncated.\n");
    }
}

void send_final_message(const Client *client, int wrong_message) {
    pthread_mutex_lock(&lock);
    printf("Multicast final messege\n");
    Client temp_client = *client; // Copy the original client
    char result[BUFFER_SIZE];
    char message_type [BUFFER_SIZE] ;//congartulation! or sorry
    //printf("%d",wrong_message);
    if (client->connected == 1) {
        if (wrong_message) 
        {
            printf("Simulating wrong message sent to client %d (Message intended for team %d).\n", client->client_id, temp_client.bet_team);
            temp_client.bet_team = (client->bet_team + 1) % 3; // Change bet_team to simulate wrong message (0 -> 1, 1 -> 2, 2 -> 0)
            // Adjust the correct group for the temporary client
             char *wrong_group = (temp_client.bet_team == 0) ? "tie" : (temp_client.bet_team == 1) ? game_state.group1 : game_state.group2;
        }
        else{
            printf("Simulating correct message sent to client %d (Message intended for team %d).\n", client->client_id, temp_client.bet_team);
        } 
        char* message_type = (temp_client.bet_team == 1 && game_state.score[0] > game_state.score[1]) ? "Congratulations!" : (temp_client.bet_team == 2 && game_state.score[1] > game_state.score[0]) ? "Congratulations!" : (temp_client.bet_team == 0 && game_state.score[0] == game_state.score[1]) ? "Congratulations!" : "Sorry!";
        printf("%s %d %d\n",message_type,client->bet_amount,temp_client.bet_team);
        snprintf(result, BUFFER_SIZE, "%s %d %d", message_type, client->bet_amount, temp_client.bet_team);

        ssize_t bytes_sent = send(client->TCP_socket, result, strlen(result), 0);
        if (bytes_sent < 0) {
            perror("Failed to send message");
        } else {
            printf("Message sent to client %d: %s\n", client->client_id, result);
        }
        pthread_mutex_unlock(&lock);
        // Close the client socket after sending the message
        sleep(10); // Ensure the message is sent before closing the socket

    }
}

void send_keep_alive_request() {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < client_count; i++) {
        if (clients[i]->connected) {
            const char *keep_alive_message = "KEEP_ALIVE";
            send(clients[i]->TCP_socket, keep_alive_message, strlen(keep_alive_message), 0);
            printf("Sent keep-alive request to client %d.\n", clients[i]->client_id);
        }
    }
    pthread_mutex_unlock(&lock);
}

void check_keep_alive() {
    pthread_mutex_lock(&lock);
    time_t current_time = time(NULL);
    for (int i = 0; i < client_count; i++) {
        if (clients[i]->connected) {
            if (difftime(current_time, clients[i]->last_keep_alive) > KEEP_ALIVE_TIMEOUT) {
                clients[i]->missed_keep_alives++;
                if (clients[i]->missed_keep_alives >= 2) {
                    printf("Client %d missed two keep-alives. Disconnecting...\n", clients[i]->client_id);  
                    const char *disconnect_message = "DISCONNECT";
                    send(clients[i]->TCP_socket, disconnect_message, strlen(disconnect_message), 0);
                    close(clients[i]->TCP_socket);
                     close(clients[i]->UDP_socket);
                    clients[i]->connected = 0;
                    free(clients[i]);
                    client_count--;
                    i--;
                }
            }
        }
    }
    pthread_mutex_unlock(&lock);
}

void* keep_alive_manager(void* arg) {
    while (1) {
        send_keep_alive_request();
        sleep(KEEP_ALIVE_INTERVAL);
        check_keep_alive();
    }
    return NULL;
}







