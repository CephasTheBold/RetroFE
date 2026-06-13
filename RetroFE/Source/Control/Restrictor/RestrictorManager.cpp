
#include "RestrictorManager.h"
#include "../../Utility/ThreadPool.h" // Include your thread pool
#include "../../Utility/Log.h"

     static constexpr char COMPONENT[] = "RestrictorManager";
     IRestrictor* RestrictorManager::gRestrictor = nullptr;

 RestrictorManager::RestrictorManager() = default;
 RestrictorManager::~RestrictorManager() = default; // The future and unique_ptr clean up themselves.

 void RestrictorManager::startInitialization() {
     // If the future is already valid, we've already started.
     if (restrictorFuture_.valid()) {
         return;
     }
     LOG_INFO(COMPONENT, "Enqueuing restrictor hardware detection to thread pool...");
     restrictorFuture_ = ThreadPool::getInstance().enqueue(&IRestrictor::create);
 }

 bool RestrictorManager::isReady() {
     return !restrictorFuture_.valid() || restrictorFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
 }

 void RestrictorManager::waitForCompletion() {
     if (!restrictorFuture_.valid()) {
         return;
     }

     restrictor_ = restrictorFuture_.get();
     gRestrictor = restrictor_.get();
 }

 IRestrictor* RestrictorManager::getGlobalRestrictor() {
     return gRestrictor;
 }