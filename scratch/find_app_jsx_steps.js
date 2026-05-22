const fs = require('fs');
const readline = require('readline');

async function run() {
  const fileStream = fs.createReadStream('C:/Users/jonas/.gemini/antigravity/brain/5c02e438-1462-44f8-83cb-135a7244547a/.system_generated/logs/transcript.jsonl');
  const rl = readline.createInterface({
    input: fileStream,
    crlfDelay: Infinity
  });

  let lineCount = 0;
  for await (const line of rl) {
    lineCount++;
    if (line.toLowerCase().includes('app.jsx')) {
      try {
        const obj = JSON.parse(line);
        console.log(`Line ${lineCount}: Step ${obj.step_index}, Type: ${obj.type}, Source: ${obj.source}, Content length: ${obj.content ? obj.content.length : 0}, Tool calls: ${obj.tool_calls ? obj.tool_calls.length : 0}`);
      } catch (e) {
        console.log(`Line ${lineCount}: Error parsing JSON, len ${line.length}`);
      }
    }
  }
}

run();
