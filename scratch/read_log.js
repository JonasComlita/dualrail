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
    if (line.includes('app.jsx')) {
      console.log(`Line ${lineCount}: len ${line.length}`);
      if (line.length > 500 && line.length < 5000) {
        console.log("Snippet:", line.substring(0, 300));
      } else if (line.length >= 5000) {
        console.log("Snippet:", line.substring(0, 200) + " ... " + line.substring(line.length - 200));
      }
    }
  }
}

run();
