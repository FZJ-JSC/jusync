#include "AnariUsdClient.h"
#include "AnariUsdMessages.h"
#include "MiddlewareLogging.h"

#include <iostream>
#include <vector>

using namespace anari_usd_middleware;

int main() {
    std::cout << "=== ANARI USD Client Test ===" << std::endl;
    
    // Test message structures
    std::cout << "\n--- Testing Message Structures ---" << std::endl;
    
    ZmqFileRequest fileReq;
    fileReq.message_type = static_cast<uint32_t>(ZmqMessageType::REQ_GET_FILE);
    fileReq.request_id = 1;
    fileReq.target_rank = 0;
    fileReq.setFilename("test.usd");
    fileReq.chunk_size = 4 * 1024 * 1024;
    
    std::cout << "File Request:" << std::endl;
    std::cout << "  Magic: 0x" << std::hex << fileReq.magic << std::dec << std::endl;
    std::cout << "  Message Type: " << MessageUtils::getMessageTypeName(fileReq.message_type) << std::endl;
    std::cout << "  Request ID: " << fileReq.request_id << std::endl;
    std::cout << "  Target Rank: " << fileReq.target_rank << std::endl;
    std::cout << "  Filename: " << fileReq.getFilename() << std::endl;
    std::cout << "  Chunk Size: " << fileReq.chunk_size << " bytes" << std::endl;
    
    ZmqFileChunk fileChunk;
    fileChunk.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK);
    fileChunk.request_id = 1;
    fileChunk.source_rank = 0;
    fileChunk.setFilename("test.usd");
    fileChunk.file_size = 1024 * 1024;
    fileChunk.chunk_offset = 0;
    fileChunk.chunk_size = 4 * 1024 * 1024;
    
    std::cout << "\nFile Chunk:" << std::endl;
    std::cout << "  Magic: 0x" << std::hex << fileChunk.magic << std::dec << std::endl;
    std::cout << "  Message Type: " << MessageUtils::getMessageTypeName(fileChunk.message_type) << std::endl;
    std::cout << "  Request ID: " << fileChunk.request_id << std::endl;
    std::cout << "  Source Rank: " << fileChunk.source_rank << std::endl;
    std::cout << "  Filename: " << fileChunk.getFilename() << std::endl;
    std::cout << "  File Size: " << fileChunk.file_size << " bytes" << std::endl;
    std::cout << "  Chunk Offset: " << fileChunk.chunk_offset << std::endl;
    std::cout << "  Chunk Size: " << fileChunk.chunk_size << " bytes" << std::endl;
    
    // Test client creation
    std::cout << "\n--- Testing Client Creation ---" << std::endl;
    AnariUsdClient client;
    std::cout << "Client created successfully" << std::endl;
    std::cout << "Connection Status: " << static_cast<int>(client.getConnectionStatus()) << std::endl;
    
    // Test message type validation
    std::cout << "\n--- Testing Message Type Validation ---" << std::endl;
    std::cout << "Valid Magic (0x55534446): " << (MessageUtils::isValidMagic(ANARI_USD_MAGIC) ? "Yes" : "No") << std::endl;
    std::cout << "Invalid Magic (0x12345678): " << (MessageUtils::isValidMagic(0x12345678) ? "Yes" : "No") << std::endl;
    
    std::cout << "\nValid Message Types:" << std::endl;
    for (uint32_t i = 1; i <= 301; i++) {
        if (MessageUtils::isValidMessageType(i)) {
            std::cout << "  " << i << ": " << MessageUtils::getMessageTypeName(i) << std::endl;
        }
    }
    
    // Test DEALER client functionality
    std::cout << "\n--- Testing DEALER Client Functionality ---" << std::endl;
    
    try {
        // Test connecting to broker
        std::cout << "Testing ConnectToBroker..." << std::endl;
        bool connectResult = client.connectToBroker("tcp://localhost:5556", 5000);
        std::cout << "ConnectToBroker result: " << (connectResult ? "Success" : "Failed") << std::endl;
        
        if (connectResult) {
            std::cout << "Connection Status: " << static_cast<int>(client.getConnectionStatus()) << std::endl;
            std::cout << "IsBrokerConnected: " << (client.isBrokerConnected() ? "Yes" : "No") << std::endl;
            
            // Test file list request
            std::cout << "\nTesting RequestFileList..." << std::endl;
            std::vector<std::string> fileList;
            bool listResult = client.requestFileList(-1, fileList, 10000);
            std::cout << "RequestFileList result: " << (listResult ? "Success" : "Failed") << std::endl;
            if (listResult) {
                std::cout << "File count: " << fileList.size() << std::endl;
                for (size_t i = 0; i < std::min(fileList.size(), size_t(5)); ++i) {
                    std::cout << "  File " << i << ": " << fileList[i] << std::endl;
                }
                if (fileList.size() > 5) {
                    std::cout << "  ... and " << (fileList.size() - 5) << " more files" << std::endl;
                }
            }
            
            // Test file request
            std::cout << "\nTesting RequestFile..." << std::endl;
            std::vector<uint8_t> fileData;
            bool fileResult = client.requestFile("test.usd", 0, fileData, 30000);
            std::cout << "RequestFile result: " << (fileResult ? "Success" : "Failed") << std::endl;
            if (fileResult) {
                std::cout << "File size: " << fileData.size() << " bytes" << std::endl;
            }
            
            // Test frame request
            std::cout << "\nTesting RequestFrame..." << std::endl;
            std::vector<std::pair<std::string, std::vector<uint8_t>>> frameFiles;
            bool frameResult = client.requestFrame(1, 0, frameFiles, 60000);
            std::cout << "RequestFrame result: " << (frameResult ? "Success" : "Failed") << std::endl;
            if (frameResult) {
                std::cout << "Frame file count: " << frameFiles.size() << std::endl;
                for (size_t i = 0; i < std::min(frameFiles.size(), size_t(3)); ++i) {
                    std::cout << "  Frame file " << i << ": " << frameFiles[i].first 
                              << " (" << frameFiles[i].second.size() << " bytes)" << std::endl;
                }
            }
            
            // Test disconnection
            std::cout << "\nTesting DisconnectFromBroker..." << std::endl;
            client.disconnectFromBroker();
            std::cout << "DisconnectFromBroker completed" << std::endl;
            std::cout << "IsBrokerConnected after disconnect: " << (client.isBrokerConnected() ? "Yes" : "No") << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "Exception during DEALER client test: " << e.what() << std::endl;
    }
    
    std::cout << "\n=== Test Complete ===" << std::endl;
    return 0;
}