// solve.js — thin JS wrapper over the native addon.
// run(input) returns the site's output tree (path -> text) for one case's input.
// All of the work happens in C (nat.node); this module only crosses the boundary.
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const nat = require('./nat.node');

export function run(input) {
  return nat.run(input);
}
