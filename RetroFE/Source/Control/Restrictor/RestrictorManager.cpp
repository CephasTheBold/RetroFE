
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

 // ADD THIS FUNCTION:
 void RestrictorManager::waitForCompletion() {
     if (restrictorFuture_.valid()) {
         // This safely transitions ownership of the unique_ptr out of the future
         restrictor_ = restrictorFuture_.get();
         if (restrictor_) {
             gRestrictor = restrictor_.get();
         }
         else {
             gRestrictor = nullptr;
         }
     }
 }

 IRestrictor* RestrictorManager::getGlobalRestrictor() {
     return gRestrictor;
 }