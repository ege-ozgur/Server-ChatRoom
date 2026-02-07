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

std::mutex g_clientsMutex;
std::unordered_map<SOCKET, std::string> g_clients;

void sendToSocket(SOCKET sock, const std::string& msg) {
    send(sock, msg.c_str(), (int)msg.size(), 0);
}

void broadcastUserList() {
    std::string userListMsg = "USERS|";
    std::lock_guard<std::mutex> lock(g_clientsMutex);

    for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
        if (it != g_clients.begin()) userListMsg += ",";
        userListMsg += it->second;
    }

    userListMsg += "\n"; 

    for (auto& pair : g_clients) {
        sendToSocket(pair.first, userListMsg);
    }
}

void broadcastMessage(const std::string& msg, SOCKET senderSocket = INVALID_SOCKET) {
    std::lock_guard<std::mutex> lock(g_clientsMutex);
    for (auto& pair : g_clients) {
        if (pair.first != senderSocket) {
            sendToSocket(pair.first, msg);
        }
    }
}

void handleClient(SOCKET clientSocket) {
    char buffer[1024];

    int bytes = recv(clientSocket, buffer, 1024, 0);
    if (bytes <= 0) {
        closesocket(clientSocket);
        return;
    }
    buffer[bytes] = '\0';
    std::string username = buffer;

    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_clients[clientSocket] = username;
    }

    std::cout << username << " connected." << std::endl;
    broadcastUserList();
    broadcastMessage("SYS|" + username + " joined the chat.\n");

    while (true) {
        bytes = recv(clientSocket, buffer, 1024, 0);
        if (bytes <= 0) break;

        buffer[bytes] = '\0';
        std::string msg(buffer);

        if (msg.rfind("DM|", 0) == 0) {
            size_t firstPipe = msg.find('|');
            size_t secondPipe = msg.find('|', firstPipe + 1);

            if (secondPipe != std::string::npos) {
                std::string targetUser = msg.substr(firstPipe + 1, secondPipe - (firstPipe + 1));
                std::string content = msg.substr(secondPipe + 1);

                SOCKET targetSocket = INVALID_SOCKET;
                {
                    std::lock_guard<std::mutex> lock(g_clientsMutex);
                    for (auto& pair : g_clients) {
                        if (pair.second == targetUser) {
                            targetSocket = pair.first;
                            break;
                        }
                    }
                }

                if (targetSocket != INVALID_SOCKET) {
                    std::string dmPacket = "DM|" + username + "|" + content;
                    sendToSocket(targetSocket, dmPacket);
                }
            }
        }
        else {
            std::string msgStr(buffer, bytes);

            while (!msgStr.empty() && (msgStr.back() == '\n' || msgStr.back() == '\r' || msgStr.back() == ' ')) {
                msgStr.pop_back();
            }

            if (!msgStr.empty()) {
                std::string publicMsg = username + ": " + msgStr + "\n";
                broadcastMessage(publicMsg, clientSocket);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_clients.erase(clientSocket);
    }

    closesocket(clientSocket);
    broadcastUserList();
    broadcastMessage("SYS|" + username + " left the chat.\n");
    std::cout << username << " disconnected." << std::endl;
}

int main() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(65432);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(serverSocket, SOMAXCONN);

    std::cout << "Server listening on 65432..." << std::endl;

    while (true) {
        SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket != INVALID_SOCKET) {
            std::thread(handleClient, clientSocket).detach();
        }
    }

    closesocket(serverSocket);
    WSACleanup();
    return 0;
}