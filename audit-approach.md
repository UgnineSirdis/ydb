# YDB Audit Logs for SOx

Here to specify approach to address the SOx requirements as applicable to the YDB code used to build the YDB cluster binaries (`ydbd`).

Major requirements are that the audit logs contain analysable information about:
- Critical actions performed by admin users
- Direct changes to the database

Mapping those requirements to the YDB vocabulary:

- **Direct changes to the database** – any operation modifying a tenant database schema objects (both metadata and data), performed under a personal user account, or under a system account with a personal account in the impersonation chain.

- **Critical action performed by admin users** – any request to the cluster API which triggers any change, i.e. excluding those returning information (read).

- **Cluster  API** – any API (both gRPC and http, public and private) outside the database API.

- **Database API** – set of APIs supporting actions over the database schema objects inside a tenant database, designed for usage in the database applications. This definition excludes APIs over technical objects assigned to the database, like for instance tablets.

To satisfy the requirements, we need to:

1. Come up with the list of all database APIs (both gRPC and http, public and private)

2. Ensure all database APIs have option to write relevant audit log records for requests from user accounts (or user in the impersonation chain)

3. Come up with the list of all cluster APIs (both gRPC and http, public and private), categorised as read/write (getting information / performing an action).

4. Ensure all APIs categorised as ‘write' have relevant audit log records coded
