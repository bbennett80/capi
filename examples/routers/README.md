# Route modules

`users.c` builds a reusable router with a shared authorization middleware.
`main.c` includes it in an API router, then includes that group under both
`/api/v1` and `/api/latest`. The router builders are freed before serving.

```sh
make
./build/capi_routers
```

In another terminal:

```sh
curl http://127.0.0.1:8080/health
curl -H 'Authorization: Bearer local-demo' http://127.0.0.1:8080/api/v1/users
curl -H 'Authorization: Bearer local-demo' http://127.0.0.1:8080/api/latest/users/42
```

Omit the Authorization header on a users endpoint to see the group's 401.
The independent health route remains public. This constant bearer value is
an illustration of middleware composition, not an authentication service.
Pass another port as an optional argument, or port 0 to select a free port.

The [router guide](../../docs/routers.md) covers snapshots, nested groups,
ownership, middleware order, and atomic inclusion.
