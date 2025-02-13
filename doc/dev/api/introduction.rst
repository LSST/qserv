.. _qserv-api-introduction:

Introduction
============

The Qserv REST API is a collection of RESTful web services that provide access to various components of the Qserv system.
The API enforces a specific interaction model between the client and the server. The following highlights are worth mentioning:

- All ``POST``, ``PUT`` and ``DELETE`` requests must be accompanied by a JSON payload.
- Responses of all but a few select services are in JSON format. Exceptions are documented in the API documentation.
- Schemas of the JSON requests and payloads are defined in the API documentation.
- The API is versioned. The version number is included in the URL path of the ``GET`` requests, and it's
  included into the JSON payload of the ``POST``, ``PUT`` and ``DELETE`` requests.
- Critical API services are protected by an authentication mechanism. The client must provide a valid
  authentication token in the JSON payload of the ``POST``, ``PUT`` and ``DELETE`` requests.
  No authentication is required for the ``GET`` requests.

The general information on the structure of the API can be found in the following document:

- :ref:`ingest-general`

The rest of the current document provides detailed information on the individual services that are available in the Qserv API.
