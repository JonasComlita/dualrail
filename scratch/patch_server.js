const fs = require('fs');
const path = require('path');

const filePath = path.join(__dirname, '../treatcode/server.ts');
let content = fs.readFileSync(filePath, 'utf-8');

// 1. Replace all "pub fn" with "fn" (starter templates)
// Since we also have "pub fn" in some other places, we want to be safe, but actually all "pub fn" in the file are inside templates!
content = content.replace(/\bpub\s+fn\b/g, 'fn');

// 2. Locate compileAndRunTrit and patch the code combination logic
const target = `  // Combine ulib_mini and user wrapper code
  const fullCode = \`\${ulibMini}\\n\${code}\`;
  fs.writeFileSync(tempTrit, fullCode, "utf-8");`;

const replacement = `  // Sanitize user code: strip import statements and replace "pub fn" with "fn"
  const sanitizedCode = code
    .replace(/^\\s*import\\s+\\w+\\s*;/gm, "// import statement removed")
    .replace(/\\bpub\\s+fn\\b/g, "fn");

  // Combine ulib_mini and user wrapper code
  const fullCode = \`\${ulibMini}\\n\${sanitizedCode}\`;
  fs.writeFileSync(tempTrit, fullCode, "utf-8");`;

if (content.includes(target)) {
  content = content.replace(target, replacement);
  console.log("Successfully patched compileAndRunTrit combination logic!");
} else {
  console.error("Target combination logic not found!");
}

fs.writeFileSync(filePath, content, 'utf-8');
console.log("Saved patched server.ts");
