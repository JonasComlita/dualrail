const fs = require('fs');
const path = require('path');
const readline = require('readline');

async function run() {
  const fileStream = fs.createReadStream('C:/Users/jonas/.gemini/antigravity/brain/5c02e438-1462-44f8-83cb-135a7244547a/.system_generated/logs/transcript.jsonl');
  const rl = readline.createInterface({
    input: fileStream,
    crlfDelay: Infinity
  });

  for await (const line of rl) {
    try {
      const obj = JSON.parse(line);
      // Check if it's a VIEW_FILE or similar step, and if the path was app.jsx
      if (obj.type === 'VIEW_FILE' && obj.content && obj.content.includes('File Path: `file:///c:/Users/jonas/Documents/trit/treatcode/app.jsx`')) {
        console.log(`Found step: ${obj.step_index}, content length: ${obj.content.length}`);
        fs.writeFileSync(path.join(__dirname, 'app_jsx_extracted.js'), obj.content);
        console.log("Successfully extracted to scratch/app_jsx_extracted.js");
        break;
      }
      // Also check if tool_calls contain app.jsx
      if (obj.tool_calls) {
        for (const tc of obj.tool_calls) {
          if (tc.name === 'view_file' && tc.args && tc.args.AbsolutePath && tc.args.AbsolutePath.endsWith('app.jsx')) {
            // Let's see if there's response/content for this step
            if (obj.content && obj.content.includes('File Path:')) {
              console.log(`Found tool_call step: ${obj.step_index}`);
            }
          }
        }
      }
    } catch (e) {
      // Ignore parse errors
    }
  }
}

run();
