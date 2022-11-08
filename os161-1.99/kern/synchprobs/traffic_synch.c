#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>

/* 
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */
// static struct semaphore *intersectionSem;

// lock
static struct lock* intersectionLock;

// condition variables
static struct cv* intersectionVehicles[4];

static int vehiclesWaiting[4];
static int vehiclesPassed[4];
volatile int dir = 0;
volatile Direction currDir = north;


/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */

void intersection_sync_init(void) {
  // initialize lock 
  intersectionLock = lock_create("trafficLock");
  if (intersectionLock == NULL) {
    panic("failed initializting lock");
  }

  for (int i = 0; i < 4; i++) {
    intersectionVehicles[i] = cv_create("vehiclesCV");
    if (intersectionVehicles[i] == NULL) {
      panic("failed initializing condition variables");
    }
  }
  /* replace this default implementation with your own implementation
   * intersectionSem = sem_create("intersectionSem",1);
   * if (intersectionSem == NULL) {
   *   panic("could not create intersection semaphore");
   * }
   */
  return;
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void intersection_sync_cleanup(void) {
  KASSERT(intersectionLock);
  
  // free lock
  lock_destroy(intersectionLock);
  
  // free condition variables
  for (int i = 0; i < 4; i++) {
    cv_destroy(intersectionVehicles[i]);
  }
  // free array
  //array_destroy(intersectionVehicles);
  /* replace this default implementation with your own implementation 
  KASSERT(intersectionSem != NULL);
  sem_destroy(intersectionSem);*/
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void intersection_before_entry(Direction origin, Direction destination) {
  KASSERT(intersectionLock);

  (void)destination;
 
  // acquire the lock now 
  lock_acquire(intersectionLock);

  if (!vehiclesWaiting[dir]) {
    currDir = origin;
    dir = (int)currDir;
  }
  if (currDir == origin)  {
    vehiclesWaiting[dir] += 1;
  } else {
    int index = (int)origin;
    vehiclesWaiting[index] += 1;
    cv_wait(intersectionVehicles[index], intersectionLock);
  }
  
  // release the lock now
  lock_release(intersectionLock);
  
  /* replace this default implementation with your own implementation 
   * (void)origin;  // avoid compiler complaint about unused parameter 
   * (void)destination; // avoid compiler complaint about unused parameter 
   * KASSERT(intersectionSem != NULL);
   * P(intersectionSem); */
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void intersection_after_exit(Direction origin, Direction destination) {
  KASSERT(intersectionLock);

  (void)destination;
  // acquire the lock now
  lock_acquire(intersectionLock);
  int index = (int)origin;
  vehiclesWaiting[index] -= 1;
  vehiclesPassed[index] += 1;
  if (vehiclesWaiting[index] == 0) {
    vehiclesPassed[index] = 0;
    for (int i = 1; i < 4; i++) {
      int j = (index + i) % 4;
      if (vehiclesWaiting[j] > 0) {
        dir = j;
        currDir = (Direction)dir;
        break;
      }
    }
  }
  cv_broadcast(intersectionVehicles[dir], intersectionLock);
  // release the lock now
  lock_release(intersectionLock);

  /* replace this default implementation with your own implementation 
   * (void)origin;  // avoid compiler complaint about unused parameter 
   * (void)destination; // avoid compiler complaint about unused parameter 
   * KASSERT(intersectionSem != NULL);
   * V(intersectionSem); */
}
