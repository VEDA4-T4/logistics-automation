#let team_name = "김치 맛있는 국밥집"
#let title = "스마트 물류 자동화 시스템"
#let sub_title = "무인의 민족"
#let authors = (
  team_name,
  "윤찬규(팀장), 이윤상, 이재백, 전제혁, 홍진기",
)

#let head = {
  [
    #text(weight: 700)[#team_name]
    #text(weight: 400)[#sub_title]
    #h(1fr)
    #text(weight: 400)[#title]

    #line(length: 100%, stroke: 0.2pt)
  ]
}

#let prompt(content, lang: "md") = {
  box(
    inset: 15pt,
    width: auto,
    fill: rgb(247, 246, 243, 50%),
  )[#text(content, font: "Cascadia Mono", size: 0.8em)]
}

#let _returns = {
  text("Returns:", font: "Cascadia Mono", weight: 600, size: 9pt)
}

#let _params = {
  text("Parameters:", font: "Cascadia Mono", weight: 600, size: 9pt)
}

#let _details = {
  text("Details:", font: "Cascadia Mono", weight: 600, size: 9pt)
}

#let enter = {
  box(
    height: 0.7em,
  )[#rect(
    fill: rgb(233, 233, 233),
    stroke: gray,
  )[#text("Enter↵", baseline: -0.7em, font: "Cascadia Mono", size: 6pt)]]
}

#let tab = {
  box(
    height: 0.7em,
    fill: rgb(233, 233, 233),
    stroke: gray,
  )[#rect(
    fill: rgb(233, 233, 233),
    stroke: gray,
  )[#text("Tab↹", baseline: -0.7em, font: "Cascadia Mono", size: 6pt)]]
}

#let project(title: "", authors: (), logo: none, body) = {
  set text(9pt, font: "Pretendard")
  set heading(numbering: "1.1")
  set page(columns: 1, numbering: "1  /  1", number-align: center, header: head, margin: 5em)
  show outline.entry.where(level: 1): it => {
    v(25pt, weak: true)
    strong(it)
  }
  show heading: it => {
    it
    v(0.5em)
  }

  place(
    top + center,
    float: true,
    scope: "parent",
    text(weight: 800, 1.75em, title),
  )
  place(
    top + center,
    float: true,
    scope: "parent",
    grid(
      columns: (1fr,) * calc.min(1, authors.len()),
      gutter: 1em,
      ..authors.map(author => align(center, author)),
    ),
  )
  set par(justify: true)
  outline(title: "목 차", depth: 5, indent: 1em)

  body
}
