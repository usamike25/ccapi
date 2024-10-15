

#include <chrono>
#include <condition_variable>
#include <cstdlib>  // for getenv
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "ccapi_cpp/ccapi_session.h"

namespace ccapi {
Logger* Logger::logger = nullptr;  // This line is needed.

class MyEventHandler : public EventHandler {
 public:
  MyEventHandler(std::mutex* mutex, std::condition_variable* cv, std::map<std::string, Event>* eventMap) : mutex_(mutex), cv_(cv), eventMap_(eventMap) {}

  bool processEvent(const Event& event, Session* session) override {
    std::cout << "Received an event: " + event.toStringPretty(2, 2) << std::endl;

    if (event.getType() == Event::Type::RESPONSE) {
      for (const auto& message : event.getMessageList()) {
        std::string correlationId = message.getCorrelationIdList().empty() ? "" : message.getCorrelationIdList().at(0);
        {
          std::lock_guard<std::mutex> lock(*mutex_);
          (*eventMap_)[correlationId] = event;
        }
        cv_->notify_all();
      }
    }

    // Handle the event 2
    if (event.getType() == Event::Type::SUBSCRIPTION_STATUS) {
      std::cout << "Received an event of type SUBSCRIPTION_STATUS:\n" + event.toStringPretty(2, 2) << std::endl;
    } else if (event.getType() == Event::Type::SUBSCRIPTION_DATA) {
      for (const auto& message : event.getMessageList()) {
        // Print the timestamp of the message
        std::cout << "Best bid and ask at " + UtilTime::getISOTimestamp(message.getTime()) + " are:" << std::endl;

        // Iterate through each element in the message
        for (const auto& element : message.getElementList()) {
          const std::map<std::string, std::string>& elementNameValueMap = element.getNameValueMap();

          // Print each element's name-value pair in a readable format
          for (const auto& pair : elementNameValueMap) {
            std::cout << "  " << pair.first << ": " << pair.second << std::endl;  // Print key and value
          }
        }
      }
    }
    return true;
  }

 private:
  std::mutex* mutex_;
  std::condition_variable* cv_;
  std::map<std::string, Event>* eventMap_;
};
}  // namespace ccapi

using namespace ccapi;
using namespace std;
using ::ccapi::Subscription;
using ::ccapi::UtilSystem;

// Function to trim whitespace from a string
std::string trim(const std::string& str) {
  const char* whitespace = " \t\n\r\f\v";
  size_t start = str.find_first_not_of(whitespace);
  size_t end = str.find_last_not_of(whitespace);
  return (start == std::string::npos) ? "" : str.substr(start, end - start + 1);
}

// Function to load environment variables from a .env file
void loadEnvFile(const std::string& filename) {
  std::ifstream file(filename);
  std::string line;

  if (!file.is_open()) {
    std::cerr << "Error opening .env file" << std::endl;
    return;
  }

  while (std::getline(file, line)) {
    // Ignore empty lines and comments
    if (line.empty() || line[0] == '#') continue;

    // Parse the key-value pair
    std::istringstream lineStream(line);
    std::string key, value;
    if (std::getline(lineStream, key, '=') && std::getline(lineStream, value)) {
      // Trim any leading/trailing whitespace
      key = trim(key);
      value = trim(value);

      // Set the environment variable using setenv (on Unix-like systems)
      setenv(key.c_str(), value.c_str(), 1);  // 1 to overwrite any existing variable
    }
  }

  file.close();
}

int main(int argc, char** argv) {
  std::cout << "start.." << std::endl;

    // Load environment variables from the .env file
    loadEnvFile("../src/order_latency_measurement/config.env");

    // Load environment variables using UtilSystem
    std::string exchange = std::getenv("EXCHANGE");
    std::string trading_pair = std::getenv("INSTRUMENT");
    std::string key = std::getenv("KUCOIN_API_KEY");
    std::string secret = std::getenv("KUCOIN_API_SECRET");
    std::string passphrase = std::getenv("KUCOIN_API_PASSPHRASE");

    std::cout << "exchange: " << exchange << std::endl;
    std::cout << "trading_pair: " << trading_pair << std::endl;
    std::cout << "key: " << key << std::endl;
    std::cout << "secret: " << secret << std::endl;

    std::map<std::string, std::string> credentials = {{"KUCOIN_API_KEY", key}, {"KUCOIN_API_SECRET", secret}, {"KUCOIN_API_PASSPHRASE", passphrase}};

    SessionOptions sessionOptions;
    SessionConfigs sessionConfigs;

    // // Set the API credentials in the session configurations
    // sessionConfigs.setCredential({{"GATEIO_API_KEY", key}, {"GATEIO_API_SECRET", secret}});

    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, Event> eventMap_;
    MyEventHandler eventHandler(&mutex_, &cv_, &eventMap_);
    Session session(sessionOptions, sessionConfigs, &eventHandler);

    // subscribe to the market data
    // Subscription subscription("gateio", "ETH_USDT", "MARKET_DEPTH", "MARKET_DEPTH_MAX=20");
    // session.subscribe(subscription);

    // latency mesurements
    std::vector<std::int64_t> latencyCreateVec;
    std::vector<std::int64_t> latencyDeleteVec;

    for (int i = 0; i < 10; ++i) {
      // Record start time for order creation
      auto startCreate = std::chrono::steady_clock::now();

      // Generate a unique correlation ID for the create order request
      std::string createCorrelationId = "create_order_" + std::to_string(i);

      // Prepare and send the create order request
      Request createRequest(Request::Operation::CREATE_ORDER, exchange, trading_pair, "rr", credentials);
      createRequest.appendParam({{"SIDE", "BUY"},
                                 {"QUANTITY", "0.05"},
                                 {
                                     "LIMIT_PRICE",
                                     "100",
                                 },
                                 {
                                     "CLIENT_ORDER_ID",
                                     "6d4eb0fb-2229-469f-873e-557dd78ac11e",
                                 }});

      createRequest.setCorrelationId(createCorrelationId);
      session.sendRequest(createRequest);

      // Wait for the create order response
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return eventMap_.find(createCorrelationId) != eventMap_.end(); });
      }

      // Record end time for order creation
      auto endCreate = std::chrono::steady_clock::now();

      // Calculate latency for order creation
      auto latencyCreate = std::chrono::duration_cast<std::chrono::milliseconds>(endCreate - startCreate).count();

      latencyCreateVec.push_back(latencyCreate);

      // Extract the order ID from the response
      std::string orderId;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const Event& createEvent = eventMap_[createCorrelationId];
        for (const auto& message : createEvent.getMessageList()) {
          orderId = message.getElementList().at(0).getValue("ORDER_ID");
        }
        eventMap_.erase(createCorrelationId);
      }

      // Record start time for order deletion
      auto startDelete = std::chrono::steady_clock::now();

      // Generate a unique correlation ID for the delete order request
      std::string deleteCorrelationId = "delete_order_" + std::to_string(i);

      // Prepare and send the delete order request
      Request deleteRequest(Request::Operation::CANCEL_ORDER, exchange, trading_pair, "rr", credentials);
      deleteRequest.appendParam({{"ORDER_ID", orderId}});
      deleteRequest.setCorrelationId(deleteCorrelationId);
      session.sendRequest(deleteRequest);

      // Wait for the delete order response
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return eventMap_.find(deleteCorrelationId) != eventMap_.end(); });
      }

      // Record end time for order deletion
      auto endDelete = std::chrono::steady_clock::now();

      // Calculate latency for order deletion
      auto latencyDelete = std::chrono::duration_cast<std::chrono::milliseconds>(endDelete - startDelete).count();

      latencyDeleteVec.push_back(latencyDelete);

      // Calculate total roundtrip latency
      auto totalLatency = latencyCreate + latencyDelete;

      // Output the latencies
      std::cout << "Iteration " << i + 1 << ":\n"
                << "Create Order Latency: " << latencyCreate << " ms\n"
                << "Delete Order Latency: " << latencyDelete << " ms\n"
                << "Total Roundtrip Latency: " << totalLatency << " ms\n"
                << std::endl;

      // Clean up the event map
      {
        std::lock_guard<std::mutex> lock(mutex_);
        eventMap_.erase(deleteCorrelationId);
      }

      // Wait one second before the next iteration
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    session.stop();

    // Calculate the average latencies

    auto sumCreate = std::accumulate(latencyCreateVec.begin(), latencyCreateVec.end(), 0);
    auto sumDelete = std::accumulate(latencyDeleteVec.begin(), latencyDeleteVec.end(), 0);

    auto avgCreate = sumCreate / latencyCreateVec.size();
    auto avgDelete = sumDelete / latencyDeleteVec.size();

    std::cout << "Average Create Order Latency: " << avgCreate << " ms\n"
              << "Average Delete Order Latency: " << avgDelete << " ms\n"
              << std::endl;
    std::cout << "Bye" << std::endl;
    return EXIT_SUCCESS;
  }