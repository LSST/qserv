
TODO
----


Finish in a scope of the current ticket DM-42005 before the X-Mas break:

- [**x**] Think about the locking mechanism of the method WorkerHttpRequest::toJson(). The method
  acquires a lock on the mutext while the request may too have a lock on the same mutex
  while processing the request in WorkerHttpRequest::execute(). This may result in a deadlock.
  Perhaps no locking is needed as all since the resulting data are not lock sencitive?
- [**x**] Finish implementing a hierachy of the HTTP-based worker requests
- [**x**] Finish implementing the request processor for these requests
- [**x**] Add the new service to the Condfiguration and Registry to allow the Controller to send requests
  to the worker via HTTP
- [**x**] Display connection parameters of  the new service on the Web Dashboard
- [ ] Document the REST services in the documentation tree.
- [ ] Manually test the new implementation externally using ``curl`` or Python's ``requests`` module.
  Think about the test cases to cover the new implementation.
- [ ] Extend the integration tests to cover the new implementation. 

Finish in a scope of a separate ticket during/after the X-Mas break:

- [ ] Implement the MessengerHttp on the Controller side of the protocol. The class will
  be providing the multiplexing API for the Controller to send requests to the worker.
  The initial implementation will be based on the simple http::AsyncReq.
- [ ] Create a parallel hierarchy of the HTTP-based request & job classes on the Controller
  side of the protocol.
- [ ] Test the new classes.
- [ ] Implement the MessengerHttp to reuse the socket connections for sending multiple requests
  to the same worker.
- [ ] Test the new implementation to ensure it works the same way as the old one.
- [ ] Remove the old implementation of the Controller - Worker protocol.

