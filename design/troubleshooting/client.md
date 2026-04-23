# Client Troubleshooting

## Table of Contents

- [Client does not appear in management / no clients in DB](#client-does-not-appear-in-management--no-clients-in-db)

## Client does not appear in management / no clients in DB

**Symptom:** Management shows no clients after install or upgrade. Kafka topics `<node>.client.main` and `<node>.managementAgent.main` are never created.

**Cause:** The nvmeibc kernel module was compiled without a reachable annotated git tag, so `git describe` returned an undescribed result and the version string was left empty (e.g. `"version": "", "build_number": "buildnumber"`). nvmeshcm reads the module version to construct the Kafka topic name for the keepalive message. With an empty version the topic lookup fails silently and no message is ever published to Kafka, so management never receives the agent keepalive that triggers client registration.

**Fix:** Before building, ensure the repository has an annotated tag reachable from the current commit, e.g.:
```bash
git tag -a v3.4.0 -m "v3.4.0"
```
`git describe` must return a non-empty string for the build system to embed a valid version.

**How to confirm:** Run on the client node:
```bash
cat /proc/nvmeibc/version
```
The output must show a non-empty `version` and a real `build_number`, not `"buildnumber"`.
