#include "ccapi_cpp/ccapi_session.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>

namespace ccapi {
Logger* Logger::logger = nullptr;  // This line is needed.

class MyEventHandler : public EventHandler {
 public:
  MyEventHandler(std::mutex* mutex, std::condition_variable* cv,
                 std::map<std::string, Event>* eventMap)
      : mutex_(mutex), cv_(cv), eventMap_(eventMap) {}

  bool processEvent(const Event& event, Session* session) override {
    std::cout << "Received an event: " + event.toStringPretty(2, 2) << std::endl;

    if (event.getType() == Event::Type::RESPONSE) {
      for (const auto& message : event.getMessageList()) {
        std::string correlationId = message.getCorrelationIdList().empty()
                                        ? ""
                                        : message.getCorrelationIdList().at(0);
        {
          std::lock_guard<std::mutex> lock(*mutex_);
          (*eventMap_)[correlationId] = event;
        }
        cv_->notify_all();
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

int main(int argc, char** argv) {

  std::cout << "start.." << std::endl;
  std::string key = "0756a95abbe3eb84e6fff97ede057b7a";
  std::string secret = "f1f4eb074accd1d4f2950748d6696df0b8c2e7fe9e9c8b8c87a26edbcaf85bf2";

  SessionOptions sessionOptions;
  SessionConfigs sessionConfigs;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<std::string, Event> eventMap_;
  MyEventHandler eventHandler(&mutex_, &cv_, &eventMap_);
  Session session(sessionOptions, sessionConfigs, &eventHandler);

  //latency mesurements
  std::vector<std::int64_t> latencyCreateVec;
  std::vector<std::int64_t> latencyDeleteVec;

  // Set the API credentials in the session configurations
  sessionConfigs.setCredential({
  {"GATEIO_API_KEY", key},
  {"GATEIO_API_SECRET", secret}
});

  for (int i = 0; i < 10; ++i) {
    // Record start time for order creation
    auto startCreate = std::chrono::steady_clock::now();

    // Generate a unique correlation ID for the create order request
    std::string createCorrelationId = "create_order_" + std::to_string(i);

    // Prepare and send the create order request
    Request createRequest(Request::Operation::CREATE_ORDER, "gateio", "ETH_USDT","", {
  {"GATEIO_API_KEY", key},
  {"GATEIO_API_SECRET", secret}
});
    createRequest.appendParam({
        {"SIDE", "BUY"},
        {"QUANTITY", "0.05"},
        {"LIMIT_PRICE", "100"}
    });
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
    auto latencyCreate =
        std::chrono::duration_cast<std::chrono::milliseconds>(endCreate - startCreate).count();

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
    Request deleteRequest(Request::Operation::CANCEL_ORDER, "gateio", "ETH_USDT", "", {
  {"GATEIO_API_KEY", key},
  {"GATEIO_API_SECRET", secret}
});
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
    auto latencyDelete =
        std::chrono::duration_cast<std::chrono::milliseconds>(endDelete - startDelete).count();

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