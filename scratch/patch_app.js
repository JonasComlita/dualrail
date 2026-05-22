const fs = require('fs');
const path = require('path');

const filePath = path.join(__dirname, '../treatcode/src/App.tsx');
let content = fs.readFileSync(filePath, 'utf-8');

// Replace all "pub fn" with "fn"
content = content.replace(/\bpub\s+fn\b/g, 'fn');

fs.writeFileSync(filePath, content, 'utf-8');
console.log("Saved patched App.tsx");
