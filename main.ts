// main.ts — thin driver: the harness runs `node main.ts`, and everything
// (read cases.json, build every case, write answers.json) happens in C.
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const nat = require('./nat.node') as {
  runAll: (inputDir: string, outputDir: string) => number;
};

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

nat.runAll(INPUT_DIR, OUTPUT_DIR);
