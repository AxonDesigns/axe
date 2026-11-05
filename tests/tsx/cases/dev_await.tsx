// --dev: a top-level await hands the module's owner back while it waits; one in a function doesn't.
await Promise.resolve();
const a = await fetchIt(await other());
async function f() {
  await g();
}
const h = async () => await g();
