.. _ingest-general:

General guidelines
==================

.. _ingest-general-request-headers:

Request headers
---------------

All (but those where it's explicitly stated otherwise) services accepting requests send with ``POST``, ``PUT`` or ``DELETE``
methods require the following HTTP header to be sent in the request's body along with the JSON request object:

.. code-block::

    Content-Type: application/json 

When requests are sent using the command line application ``curl`` then the following option must be used:

.. code-block:: bash
    
    curl <url> -X <method> -H "Content-Type: application/json"

In this case a JSON object can be specified using one of the following methods:

.. code-block:: bash

    echo '{...}' | curl <url> -X <method> -H <header> -d@-
    curl <url> -X <method> -H <header> -d '{...}'

Where ``{...}`` represents a JSON object with details of the request. The object may not be required for some requests.
Specific requirements for this will be mentioned in each service. If the object is not required for a for particular
request then the body is allowed to be empty, or it could be an empty JSON  object ``{}``.

All (no exception) services return results and errors as JSON  objects as explained in the next subsection below.

.. _ingest-general-error-reporting:

Error reporting when calling the services
-----------------------------------------

.. note:

    The error reporting mechanism implemented in the System serves as a foundation for building reliable workflows.

All services explained in the document adhere to the usual conventions adopted by the Web community for designing and using the REST APIs. In particular, HTTP code 200 is returned if a request is well-formed and accepted by the corresponding service. Any other code shall be treated as an error. However, the implementation of the System further extends the error reporting mechanism by guaranteeing that all services did the fine-grain error reporting in the response objects. All services of the API are guaranteed to return an JSON object if the HTTP code is 200. The objects would have the following mandatory attributes (other attributes depend on a request):

.. code-block::

    {   "success" :   <number>,
        "error" :     <string>,
        "error_ext" : <object>,
        ...
    }

**Note**: depending on the service, additional attributes may be present in the response object.

Therefore, even if a request is completed with HTTP code ``200``, a client (a workflow) must inspect the above-mentioned
fields in the returned object. These are the rules for inspecting the status attributes:

- Successful completion of a request is indicated by having success=1 in the response. In these cases, the other
  two fields should be ignored.
- Otherwise, a human-readable explanation of a problem would be found in the error field.
- Request-specific extended information on errors is optionally provided in the error_ext field.

Optional warnings
^^^^^^^^^^^^^^^^^

**Note**: Warnings were introduced as of version ``12`` of the API.

REST services may also return the optional attribute ``warning`` a caller about potential problems with a request.
The very presence of such a warning doesn't necessarily mean that the request failed. Users are still required
to use the above-described error reporting mechanism for inspecting the completion status of requests.
Warnings carry the additional information that may be present in any response regardless if it succeeded or not.
It's up to a user to interpret this information based on a specific request and the context it was made.

Here is what to expect within the response object if the warning was reported:

.. code-block::

    {   "success" : <number>,
        "warning" : <string>,
        ...
    }

.. _ingest-general-auth:

Authorization and authentication
--------------------------------

All services accepting requests sent with ``POST``, ``PUT`` or ``DELETE`` methods require the following attribute
to be present in the request object:

``auth_key`` : *string*
  The authentication key that is required for an operation. The key is used to prevent unauthorized access to the service.

Certain requests (where it's specificly stated by the description of the service) may require the elevated privileges
to be specified in the following attribute:

``admin_auth_key`` : *string*
  The Administrator-level authentication key that is required for an operation. The key is used to prevent unauthorized
  access to the service that will modify existing data visible to Qserv users.

.. _ingest-general-versioning:

Protocol Versioning
-------------------

The API adheres to the optional version control mechanism introduced in:

- https://rubinobs.atlassian.net/browse/DM-35456 

Workflow developers are encouraged to use the mechanism to reinforce the integrity of the applications.

There are two ways the workflows can use the version numbers:

- *pull mode*: Ask the Replication Controller explicitly what version it implements and cross-check the returned
  version versus a number expected by the application.
- *push mode*: Pass the expected version number as a parameter when calling services and let
  the services verify if that version matches one of the frontend implementations.

Workflow developers are free to use neither, either of two, or both methods of reinforcing their applications.

Pull mode
^^^^^^^^^

To support the first scenario, the API provides a special metadata service that will return
the version number (along with some other information on the frontend):

..  list-table::
    :widths: 10 90
    :header-rows: 0

    * - ``GET``
      - ``/meta/version``

The request object for this request is not required, or it could be an empty JSON object ``{}``.
In case of its successful completion, the service will return a JSON object that will include
the following attributes (along with the other standard attributed that are used for error reporting):

.. code-block::

    {   "kind" :                    <string>,
        "name" :                    <string>,
        "id" :                      <number>,
        "instance_id" :             <string>,
        "version" :                 <number>,
        "database_schema_version" : <number>,
        "success" :                 <number>,
        "warning" :                 <string>,
        "error" :                   <string>,
        "error_ext" :               <object>
    }

Where, the service-specific attributes are:

``kind`` : *string*
  The name of the service. The following name is always reported:

  .. code-block::

    replication-controller

``name`` : *string*
  The unique name of the frontend within a given Qserv. The current implementation will always return:

  .. code-block::

    http

``id`` : *number*
  A unique identifier of the Replication Controller. The number returned here may vary.

``instance_id`` : *string*
  An identifier of the Qserv instance. A value of the attribute depends on a particular deployment of Qserv.

``version`` : *number*
  The current version number of the API.

``database_schema_version`` : *number*
  The schema version number of the Replication System's Database.

Example:

.. code-block:: json

    {   "kind" :                    "replication-controller",
        "id" :                      "9037c818-4820-4b5e-9219-edbf971823b2",
        "instance_id" :             "qserv_proj",
        "version" :                 27,
        "database_schema_version" : 14,
        "success" :                 1,
        "error" :                   "",
        "error_ext" :               {},
        "warning" :                 ""
    }

Push mode
^^^^^^^^^

In the case of the second scenario, an application will pass the desired version number as
a request parameter. The number would be a part of the request's query for the method. For example,
the following request for checking the status of the ongoing query might look like this:

.. code-block:: bash

   curl 'http://localhost:25004/trans/contrib/1234?version=35' -X GET

For other HTTP methods used by the API, the number is required to be provided within the body
of a request as shown below:

.. code-block:: bash

   curl 'http://localhost:25004/trans/contrib' -X POST \
     -H 'Content-Type: application/json' \
     -d'{"version":35, ..."}'

If the number does not match expectations, such a request will fail and the service return the following
response. Here is an example of what will happen if the wrong version number ``29`` is specified instead
of ``35`` (as per the current version of the API):

.. code-block:: json

    {   "success" : 0,
        "error" :   "The requested version 29 of the API is not in the range supported by the service.",
        "error_ext": {
            "max_version" : 35,
            "min_version" : 32
        },
        "warning" : ""
    }

.. _ingest-general-binary-encoding:

Binary encoding of the data in JSON
-----------------------------------

The API supports encoding of the binary data into JSON. The encoding specification is provided as a parameter
``binary_encoding`` when calling several services. The parameter may be optional and if not provided, the default
value is ``hex``. The parameter is used by the services and by the client applications in two different ways:

- When a client is sending data to a service, the client is required to tell the service how the binary data are encoded.
  The service would invoke the corresponding decoding algorithm to decode the data into the original representation.

- A service designed for sending data to a client is expected to get the name of the desired encoding
  algorithm in a request to the service. The service would then encode the binary data into the JSON object
  using the specified algorithm.
 
The following options for the values of the parameter are allowed in the current version of the API:

- ``hex`` - for serializing each byte into the hexadecimal format of 2 ASCII characters per each byte
  of the binary data, where the encoded characters will be in a range of ``0 .. F``. In this case,
  the encoded value will be packaged into the JSON string.
- ``b64`` - for serializing bytes into a string using the ``Base64`` algorithm with padding (to ensure 4-byte alignment).
- ``array`` - for serializing bytes into the JSON array of numbers in a range of ``0 .. 255``.

Here is an example of the same sequence of 4-bytes encoded into the hexadecimal format:

.. code-block::

    0A11FFD2

The array representation of the same binary sequence would look like this:

.. code-block:: json

    [10,17,255,210]

MySQL types (regardless of the case) that include the following keywords are treated as binary:

.. code-block:: sql

    BIT 
    BINARY 
    BLOB 

For example, these are the binary types:

.. code-block:: sql

    BIT(1)
    BINARY(8)
    VARBINARY(16)
    TINYBLOB
    BLOB
    MEDIUMBLOB
    LONGBLOB


.. _ingest-general-base-table-names:

Base versus final table names
-----------------------------

In descriptions of several services, the documentation uses an adjective *base* when referring to tables affected
by requests to the services. In reality, those *base* tables are exactly the names of the Qserv tables as they are seen
by Qserv users. In the distributed realm of Qserv each such table is collectively represented by many *final* tables
distributed across Qserv worker nodes. The names of the *final* tables depend on the table type:

- *regular* (fully replicated) tables have the same name as the *base* table
- *partitioned* (chunked) tables have names constructed using the *base* name and the chunk numbers and values
  of the overlap attribute of the table.

Formally, the names of the *final* tables are constructed as follows:

.. code-block::

    <final-table-name> = <base-name> | <base-name>_<chunk> | <base-name>FullOverlap_<chunk>

For example:

.. code-block::

    Filter
    Object_1234
    ObjectFullOverlap_1234
