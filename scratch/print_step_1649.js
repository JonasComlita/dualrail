const fs = require('fs');
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
      if (obj.step_index === 1649) {
        console.log("Step 1649 Content:");
        console.log(obj.content);
        break;
      }
    } catch (e) {}
  }
}

run();
