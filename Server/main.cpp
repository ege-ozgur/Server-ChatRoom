#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <sstream>

#pragma comment(lib, "Ws2_32.lib")

// the mutex protects access to the g_clients map, which holds the mapping of client sockets to their usernames
std::mutex g_clientsMutex;
// g_clients maps client sockets to their corresponding usernames
std::unordered_map<SOCKET, std::string> g_clients;

// this is a helper function to send a message to a specific socket
void sendToSocket(SOCKET sock, const std::string& msg) {
    send(sock, msg.c_str(), (int)msg.size(), 0);
}

// this function constructs a message containing the list of connected users and sends it to all clients
void broadcastUserList() {
	std::string userListMsg = "USERS|"; // the message starts with "USERS|" followed by a comma-separated list of usernames
    std::lock_guard<std::mutex> lock(g_clientsMutex);

	// here we iterate through the g_clients map and append each username to the userListMsg string, separated by commas
    for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
        if (it != g_clients.begin()) userListMsg += ",";
        userListMsg += it->second;
    }

    userListMsg += "\n"; 

	// finally, we send the userListMsg to all connected clients
    for (auto& pair : g_clients) {
        sendToSocket(pair.first, userListMsg);
    }
}

// this function broadcasts a message to all clients except the sender (if specified)
void broadcastMessage(const std::string& msg, SOCKET senderSocket = INVALID_SOCKET) {
	// we lock the g_clientsMutex to safely access the g_clients map while iterating through it
    std::lock_guard<std::mutex> lock(g_clientsMutex);
	// we iterate through the g_clients map and send the message to each client socket, except for the sender's socket if it's specified
    for (auto& pair : g_clients) {
        if (pair.first != senderSocket) {
            sendToSocket(pair.first, msg);
        }
    }
}

// this function handles communication with a single client. It receives messages from the client and processes them accordingly like broadcasting public messages, sending direct messages, updating the user list
void handleClient(SOCKET clientSocket) {
    char buffer[1024];

	// the first message from the client is expected to be the username, so we receive it and store it in the g_clients map
    int bytes = recv(clientSocket, buffer, 1024, 0);
    if (bytes <= 0) {
        closesocket(clientSocket);
        return;
    }
    buffer[bytes] = '\0';
	// we convert the received username into a string and store it in the g_clients map with the client socket as the key
    std::string username = buffer;
    {
		// we lock the g_clientsMutex to safely modify the g_clients map while adding the new client and their username
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_clients[clientSocket] = username;
    }
	// we print a message to the console indicating that the user has connected, then we broadcast the updated user list and a system message announcing that the user has joined the chat
    std::cout << username << " connected." << std::endl;
    broadcastUserList();
	// we send a system message to all clients announcing that the new user has joined the chat
    broadcastMessage("SYS|" + username + " joined the chat.\n");

	// now we enter a loop to continuously receive messages from the client until they disconnect
    while (true) {
        bytes = recv(clientSocket, buffer, 1024, 0);
		// if recv returns 0 or a negative value, it means the client has disconnected or an error occurred, so we break out of the loop to clean up
        if (bytes <= 0) {
            break;
        }

		// we null-terminate the received message and convert it into a string for easier processing
        buffer[bytes] = '\0';
        std::string msg(buffer);

		// if the message starts with "DM|", it indicates a direct message, so we parse the target username and message content, find the target client's socket, and send the direct message to that client
        if (msg.rfind("DM|", 0) == 0) {
			// we look for the first and second pipe characters to extract the target username and message content from the direct message format "DM|targetUser|messageContent"
            size_t firstPipe = msg.find('|');
            size_t secondPipe = msg.find('|', firstPipe + 1);

			// if we successfully find both pipe characters, we proceed to extract the target username and message content
            if (secondPipe != std::string::npos) {
                std::string targetUser = msg.substr(firstPipe + 1, secondPipe - (firstPipe + 1));
                std::string content = msg.substr(secondPipe + 1);

                SOCKET targetSocket = INVALID_SOCKET;
                {
					// we lock the g_clientsMutex to safely access the g_clients map while searching for the target user's socket
                    std::lock_guard<std::mutex> lock(g_clientsMutex);
					// we iterate through the g_clients map to find the socket corresponding to the target username
                    for (auto& pair : g_clients) {
						// if we find a match for the target username, we store the corresponding socket and break out of the loop
                        if (pair.second == targetUser) {
                            targetSocket = pair.first;
                            break;
                        }
                    }
                }
				// if we found a valid target socket, we construct the direct message in the format "DM|senderUsername|messageContent" and send it to the target client
                if (targetSocket != INVALID_SOCKET) {
                    std::string dmPacket = "DM|" + username + "|" + content;
                    sendToSocket(targetSocket, dmPacket);
                }
            }
        }
		// if the message does not start with "DM|", we treat it as a public message, so we construct a message in the format "senderUsername: messageContent" and broadcast it to all clients except the sender
        else {
            std::string msgStr(buffer, bytes);

            while (!msgStr.empty() && (msgStr.back() == '\n' || msgStr.back() == '\r' || msgStr.back() == ' ')) {
                msgStr.pop_back();
            }
			// if the message string is not empty after trimming, we construct the public message and broadcast it to all clients except the sender
            if (!msgStr.empty()) {
                std::string publicMsg = username + ": " + msgStr + "\n";
                broadcastMessage(publicMsg, clientSocket);
            }
        }
    }

	// when the client disconnects, we remove them from the g_clients map, close their socket, broadcast the updated user list and a system message announcing that they have left the chat, and print a message to the console
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_clients.erase(clientSocket);
    }

	// we close the client's socket to clean up resources
    closesocket(clientSocket);
    broadcastUserList();
    broadcastMessage("SYS|" + username + " left the chat.\n");
    std::cout << username << " disconnected." << std::endl;
}

// the main function initializes Winsock, creates a server socket, binds it to a port, and listens for incoming connections. For each accepted connection, it spawns a new thread to handle communication with that client using the handleClient function
int main() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

	// we create a TCP socket using the socket function, specifying the address family, socket type, and protocol
    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(65432); // the server will listen on port 65432
    serverAddr.sin_addr.s_addr = INADDR_ANY;

	// we bind the server socket to the specified address and port, and then we start listening for incoming connections with a maximum backlog of SOMAXCONN
    bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(serverSocket, SOMAXCONN);

	// we print a message to the console indicating that the server is listening for connections
    std::cout << "Server listening on 65432..." << std::endl;

	// we enter an infinite loop to accept incoming client connections. For each accepted connection, we spawn a new thread that calls the handleClient function to manage communication with that client
    while (true) {
        SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket != INVALID_SOCKET) {
            std::thread(handleClient, clientSocket).detach();
        }
    }
	// when we shut down the server, we close the server socket and clean up Winsock resources
    closesocket(serverSocket);
    WSACleanup();
    return 0;
}