var paragraphCount = 0;
self.document.getElementById("more").addEventListener("click", function () {
    var paragraph = document.createElement("p");
    paragraphCount += 1;
    paragraph.appendChild(document.createTextNode("展开后的中文正文，第 " + paragraphCount + " 段。"));
    document.getElementById("story").appendChild(paragraph);
});
