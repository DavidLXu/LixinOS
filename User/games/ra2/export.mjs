// Use the upstream local asset loader for both freely supplied and retail data.
import { createAssetHandler } from 'asset-handler';
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
const [assets, out] = process.argv.slice(2);
const classic = process.env.RA2_DIR;
const handler = createAssetHandler(classic);
function get(url) {
  let body, status;
  handler({url}, {setHeader() {}, set statusCode(n) {status=n;}, end(data) {body=Buffer.from(data);}});
  if (!body || status !== 200) throw Error(url + ' ' + (body || status));
  return body;
}
const mapName = process.env.RA2_MAP || (classic ? 'bloodfeud' : 'provinggrounds');
const map = JSON.parse(get('/map/'+encodeURIComponent(mapName)));
const rules = JSON.parse(get('/rules'));
const ai = JSON.parse(get('/ai')).aimd;
writeFileSync(join(out,'match.json'),JSON.stringify({mapName,classic:!!classic,map,rules,ai}));
const types = [...Object.keys(rules.buildings), ...Object.keys(rules.units)];
writeFileSync(join(assets,'types.json'),JSON.stringify(types));
for (const type of types) {
  writeFileSync(join(assets,type+'.atlas'),get('/sprite/techno/'+type+'?theater='+encodeURIComponent(map.theater)));
}
writeFileSync(join(assets,'terrain.atlas'),get('/terrain/'+encodeURIComponent(map.theater)+'?map='+encodeURIComponent(mapName)));
