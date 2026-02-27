import tseslint from "@typescript-eslint/eslint-plugin";
import tsparser from "@typescript-eslint/parser";

export default [
  {
    files: ["worker/**/*.ts"],
    languageOptions: {
      parser: tsparser,
      parserOptions: {
        ecmaVersion: 2022,
        sourceType: "module",
      },
    },
    plugins: {
      "@typescript-eslint": tseslint,
    },
    rules: {
      // No free-form console output — use the log() helper which emits structured JSON.
      // Remediation: replace console.log/console.error with log("EVENT", { ...fields })
      "no-console": "error",

      // No implicit any — forces explicit types at boundaries.
      // Remediation: annotate the parameter or return type explicitly.
      "@typescript-eslint/no-explicit-any": "error",

      // No unused variables — keeps the worker lean.
      "@typescript-eslint/no-unused-vars": ["error", { argsIgnorePattern: "^_" }],
    },
  },
];
