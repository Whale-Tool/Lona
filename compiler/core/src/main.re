open Node;

let exit = message => {
  Js.log(message);
  %bs.raw
  {|process.exit(1)|};
};

let scanResult =
  switch (CommandLine.Arguments.scan(Process.argv |> Array.to_list)) {
  | scanResult => scanResult
  | exception (CommandLine.Command.Unknown(message)) => exit(message)
  };
let options = scanResult.options;

[@bs.val] [@bs.module "fs-extra"] external ensureDirSync: string => unit = "";

[@bs.val] [@bs.module "fs-extra"]
external copySync: (string, string) => unit = "";

[@bs.module] external getStdin: unit => Js_promise.t(string) = "get-stdin";

let concat = (base, addition) => Path.join([|base, addition|]);
let replaceFileExtension = (path, ~oldExtension, ~newExtension) =>
  Path.join2(
    Path.dirname(path),
    Path.basename_ext(path, oldExtension) ++ newExtension,
  );

let version =
  Fs.readFileSync(
    concat([%bs.raw {| __dirname |}], "../package.json"),
    `utf8,
  )
  |> Js.Json.parseExn
  |> Json.Decode.at(["version"], Json.Decode.string);

let annotation = "// Generated by Lona Compiler ";

/* TODO: Detect version already in file and swap it for new version */
let annotate = contents =>
  contents |> Js.String.includes(annotation) ?
    contents : annotation ++ version ++ "\n\n" ++ contents;

let ensureDirAndWriteFile = (path, contents) => {
  ensureDirSync(Path.dirname(path));
  Fs.writeFileSync(path, contents, `utf8);
};

let writeAnnotatedFile = (path, contents) =>
  ensureDirAndWriteFile(path, annotate(contents));

let platformId = target =>
  switch (target) {
  | Types.JavaScript =>
    switch (options.javaScript.framework) {
    | JavaScriptOptions.ReactDOM => Types.ReactDOM
    | JavaScriptOptions.ReactNative => Types.ReactNative
    | JavaScriptOptions.ReactSketchapp => Types.ReactSketchapp
    }
  | Types.Swift =>
    switch (options.swift.framework) {
    | SwiftOptions.UIKit => Types.IOS
    | SwiftOptions.AppKit => Types.MacOS
    }
  | Types.Xml =>
    /* TODO: Replace when we add android */
    Types.IOS
  | Types.Reason => Types.ReasonCompiler
  };

let getTargetExtension =
  fun
  | Types.JavaScript => ".js"
  | Swift => ".swift"
  | Xml => ".xml"
  | Reason => ".re";

let formatFilename = (target, filename) =>
  switch (target) {
  | Types.Xml
  | Types.JavaScript
  | Types.Reason => Format.camelCase(filename)
  | Types.Swift => Format.upperFirst(Format.camelCase(filename))
  };

let targetExtension = target => getTargetExtension(target);

let renderColors = (target, config: Config.t) =>
  switch (target) {
  | Types.Swift => Swift.Color.render(config)
  | JavaScript => JavaScript.Color.render(config.colorsFile.contents)
  | Xml => Xml.Color.render(config.colorsFile.contents)
  | Reason => exit("Reason not supported")
  };

let renderTextStyles = (target, config: Config.t) =>
  switch (target) {
  | Types.Swift => Swift.TextStyle.render(config)
  | JavaScript =>
    JavaScriptTextStyle.render(
      config.options.javaScript,
      config.colorsFile.contents,
      config.textStylesFile.contents,
    )
  | _ => ""
  };

let renderShadows = (target, config: Config.t) =>
  switch (target) {
  | Types.Swift => SwiftShadow.render(config)
  | JavaScript =>
    JavaScriptShadow.render(
      config.options.javaScript,
      config.colorsFile.contents,
      config.shadowsFile.contents,
    )
  | _ => ""
  };

/* TODO: Update this. SwiftTypeSystem.render now returns a different format */
let convertTypes = (target, contents) => {
  let json = contents |> Js.Json.parseExn;
  switch (target) {
  | Types.Swift =>
    let importStatement =
      switch (options.swift.framework) {
      | AppKit => "import AppKit\n\n"
      | UIKit => "import UIKit\n\n"
      };
    let types =
      json
      |> TypeSystem.Decode.typesFile
      |> SwiftTypeSystem.render(options.swift)
      |> List.map((convertedType: SwiftTypeSystem.convertedType) =>
           convertedType.contents
         )
      |> Format.joinWith("\n\n");
    importStatement ++ types;
  | Types.Reason =>
    let types = json |> TypeSystem.Decode.typesFile;
    ReasonTypeSystem.renderToString(types);
  | _ => exit("Can't generate types for target")
  };
};

let convertLogic = (target, config, contents) =>
  switch (target) {
  | Types.Xml => "Can't convert Logic to XML"
  | Types.Reason => "Converting Logic to Reason isn't supported yet"
  | Types.JavaScript =>
    contents |> LogicJavaScript.convert(config) |> JavaScriptRender.toString
  | Types.Swift =>
    let importStatement =
      switch (options.swift.framework) {
      | AppKit => "import AppKit\n\n"
      | UIKit => "import UIKit\n\n"
      };
    let code = contents |> LogicSwift.convert(config) |> SwiftRender.toString;

    importStatement ++ code;
  };

let convertColors = (target, config: Config.t) =>
  renderColors(target, config);

let convertTextStyles = (target, config: Config.t) =>
  renderTextStyles(target, config);

let convertShadows = (target, config: Config.t) =>
  renderShadows(target, config);

exception ComponentNotFound(string);

let getComponentRelativePath =
    (config: Config.t, sourceComponent, importedComponent) => {
  let sourcePath =
    Node.Path.dirname(Config.Find.componentPath(config, sourceComponent));
  let importedPath = Config.Find.componentPath(config, importedComponent);
  let relativePath =
    Node.Path.relative(~from=sourcePath, ~to_=importedPath, ());
  Js.String.startsWith(".", relativePath) ?
    relativePath : "./" ++ relativePath;
};

let getAssetRelativePath = (config: Config.t, sourceComponent, importedPath) => {
  let sourcePath =
    Node.Path.dirname(Config.Find.componentPath(config, sourceComponent));
  let importedPath = Node.Path.join2(config.workspacePath, importedPath);
  let relativePath =
    Node.Path.relative(~from=sourcePath, ~to_=importedPath, ());
  Js.String.startsWith(".", relativePath) ?
    relativePath : "./" ++ relativePath;
};

let convertComponent = (config: Config.t, target, filename: string) => {
  let contents = Fs.readFileSync(filename, `utf8);
  let parsed = contents |> Js.Json.parseExn;
  let name = Node.Path.basename_ext(filename, ".component");

  switch (target) {
  | Types.JavaScript =>
    JavaScriptComponent.generate(
      options.javaScript,
      name,
      Node.Path.relative(
        ~from=Node.Path.dirname(filename),
        ~to_=config.colorsFile.path,
        (),
      ),
      Node.Path.relative(
        ~from=Node.Path.dirname(filename),
        ~to_=config.shadowsFile.path,
        (),
      ),
      Node.Path.relative(
        ~from=Node.Path.dirname(filename),
        ~to_=config.textStylesFile.path,
        (),
      ),
      config,
      Node.Path.relative(
        ~from=Node.Path.dirname(filename),
        ~to_=config.workspacePath,
        (),
      ),
      getComponentRelativePath(config, name),
      getAssetRelativePath(config, name),
      parsed,
    )
    |> JavaScript.Render.toString
  | Swift =>
    let result =
      Swift.Component.generate(config, options, options.swift, name, parsed);
    result |> Swift.Render.toString;
  | _ => exit("Unrecognized target")
  };
};

let copyStaticFiles = (target, outputDirectory) =>
  switch (target) {
  | Types.Swift =>
    let staticFiles =
      ["TextStyle", "CGSize+Resizing", "LonaViewModel"]
      @ (
        switch (options.swift.framework) {
        | UIKit => ["LonaControlView", "Shadow"]
        | AppKit => ["LNATextField", "LNAImageView", "NSImage+Resizing"]
        }
      );

    let frameworkExtension =
      switch (options.swift.framework) {
      | AppKit => "appkit"
      | UIKit => "uikit"
      };

    /* Some static files are generated differently depending on version */
    let versionString = filename =>
      switch (filename, options.swift.framework, options.swift.swiftVersion) {
      | ("TextStyle", UIKit, V4) => ".4"
      | ("TextStyle", UIKit, V5) => ".5"
      | _ => ""
      };

    staticFiles
    |> List.iter(file =>
         copySync(
           concat(
             [%bs.raw {| __dirname |}],
             "static/swift/"
             ++ file
             ++ "."
             ++ frameworkExtension
             ++ versionString(file)
             ++ ".swift",
           ),
           concat(outputDirectory, file ++ ".swift"),
         )
       );
  | Types.JavaScript =>
    switch (options.javaScript.framework) {
    | ReactDOM =>
      let staticFiles = [
        "createActivatableComponent",
        "createFocusWrapper",
        "focusUtils",
      ];
      staticFiles
      |> List.iter(file =>
           copySync(
             concat(
               [%bs.raw {| __dirname |}],
               "static/javaScript/" ++ file ++ ".js",
             ),
             concat(outputDirectory, "utils/" ++ file ++ ".js"),
           )
         );
    | _ => ()
    }
  | _ => ()
  };

let findContentsAbove = contents => {
  let lines = contents |> Js.String.split("\n");
  let index =
    lines
    |> Js.Array.findIndex(line =>
         line |> Js.String.includes("LONA: KEEP ABOVE")
       );
  switch (index) {
  | (-1) => None
  | _ =>
    Some(
      (
        lines
        |> Js.Array.slice(~start=0, ~end_=index + 1)
        |> Js.Array.joinWith("\n")
      )
      ++ "\n\n",
    )
  };
};

let findContentsBelow = contents => {
  let lines = contents |> Js.String.split("\n");
  let index =
    lines
    |> Js.Array.findIndex(line =>
         line |> Js.String.includes("LONA: KEEP BELOW")
       );
  switch (index) {
  | (-1) => None
  | _ =>
    Some(
      "\n" ++ (lines |> Js.Array.sliceFrom(index) |> Js.Array.joinWith("\n")),
    )
  };
};

let convertWorkspace = (target, workspace, output) => {
  let fromDirectory = Path.resolve(workspace, "");
  let toDirectory = Path.resolve(output, "");

  Config.load(platformId(target), options, workspace, toDirectory)
  |> Js.Promise.then_((config: Config.t) => {
       ensureDirSync(toDirectory);

       let userTypes = config.userTypesFile.contents;

       let colorsOutputPath =
         switch (target) {
         | JavaScript =>
           let outputFilePath =
             Config.Workspace.outputPathForWorkspaceFile(
               config,
               ~workspaceFile=config.colorsFile.path,
             );
           replaceFileExtension(
             outputFilePath,
             ~oldExtension=".json",
             ~newExtension=".js",
           );
         | _ =>
           concat(
             toDirectory,
             formatFilename(target, "Colors") ++ targetExtension(target),
           )
         };

       let textStylesOutputPath =
         switch (target) {
         | JavaScript =>
           let outputFilePath =
             Config.Workspace.outputPathForWorkspaceFile(
               config,
               ~workspaceFile=config.textStylesFile.path,
             );
           replaceFileExtension(
             outputFilePath,
             ~oldExtension=".json",
             ~newExtension=".js",
           );
         | _ =>
           concat(
             toDirectory,
             formatFilename(target, "TextStyles") ++ targetExtension(target),
           )
         };

       let shadowsOutputPath =
         switch (target) {
         | JavaScript =>
           let outputFilePath =
             Config.Workspace.outputPathForWorkspaceFile(
               config,
               ~workspaceFile=config.shadowsFile.path,
             );
           replaceFileExtension(
             outputFilePath,
             ~oldExtension=".json",
             ~newExtension=".js",
           );
         | _ =>
           concat(
             toDirectory,
             formatFilename(target, "Shadows") ++ targetExtension(target),
           )
         };

       writeAnnotatedFile(colorsOutputPath, renderColors(target, config));
       writeAnnotatedFile(
         textStylesOutputPath,
         renderTextStyles(target, config),
       );

       if (target == Types.Swift || target == Types.JavaScript) {
         writeAnnotatedFile(
           shadowsOutputPath,
           renderShadows(target, config),
         );
       };

       if (target == Types.Swift) {
         userTypes
         |> UserTypes.TypeSystem.toTypeSystemFile
         |> SwiftTypeSystem.render(options.swift)
         |> List.iter((convertedType: SwiftTypeSystem.convertedType) => {
              let importStatement =
                switch (options.swift.framework) {
                | AppKit => "import AppKit\n\n"
                | UIKit => "import UIKit\n\n"
                };
              let outputPath =
                concat(
                  toDirectory,
                  formatFilename(target, convertedType.name)
                  ++ targetExtension(target),
                );
              writeAnnotatedFile(
                outputPath,
                importStatement ++ convertedType.contents,
              );
            });
       };

       if (target == Types.Swift) {
         config.logicFiles
         |> List.iter((file: Config.file(LogicAst.syntaxNode)) => {
              let filename =
                formatFilename(
                  target,
                  Path.basename_ext(file.path, ".logic"),
                )
                ++ getTargetExtension(target);
              let dirname = Path.dirname(file.path);
              let outputPath =
                Config.Workspace.outputPathForWorkspaceFile(
                  config,
                  ~workspaceFile=Path.join2(dirname, filename),
                );

              let converted = convertLogic(target, config, file.contents);

              writeAnnotatedFile(outputPath, converted);
            });
       };

       copyStaticFiles(target, toDirectory);

       let successfulComponentNames =
         config.componentPaths
         |> List.filter(file =>
              switch (options.filterComponents) {
              | Some(value) => Js.Re.test(file, Js.Re.fromString(value))
              | None => true
              }
            )
         |> List.map(file => {
              let fromRelativePath =
                Path.relative(~from=fromDirectory, ~to_=file, ());
              let toRelativePath =
                concat(
                  Path.dirname(fromRelativePath),
                  Path.basename_ext(fromRelativePath, ".component"),
                )
                ++ targetExtension(target);
              let outputPath = Path.join([|toDirectory, toRelativePath|]);
              Js.log(
                Path.join([|workspace, fromRelativePath|])
                ++ "=>"
                ++ Path.join([|output, toRelativePath|]),
              );
              switch (convertComponent(config, target, file)) {
              | exception (Json_decode.DecodeError(reason)) =>
                Js.log("Failed to decode " ++ file);
                Js.log(reason);
                None;
              | exception (Decode.UnknownParameter(name)) =>
                Js.log("Unknown parameter: " ++ name);
                None;
              | exception (Decode.UnknownExprType(name)) =>
                Js.log("Unknown expr name: " ++ name);
                None;
              | exception e =>
                Js.log("Unknown error");
                Js.log(e);
                None;
              | contents =>
                ensureDirSync(Path.dirname(outputPath));
                let (contentsAbove, contentsBelow) =
                  switch (Fs.readFileAsUtf8Sync(outputPath)) {
                  | existing => (
                      findContentsAbove(existing),
                      findContentsBelow(existing),
                    )
                  | exception _ => (None, None)
                  };
                let contents =
                  switch (contentsAbove) {
                  | Some(contentsAbove) => contentsAbove ++ contents
                  | None => contents
                  };
                let contents =
                  switch (contentsBelow) {
                  | Some(contentsBelow) => contents ++ contentsBelow
                  | None => contents
                  };
                Fs.writeFileSync(outputPath, annotate(contents), `utf8);

                Some(Path.basename_ext(fromRelativePath, ".component"));
              };
            })
         |> Sequence.compact;

       if (target == Types.Swift
           && options.swift.framework == UIKit
           && options.swift.generateCollectionView) {
         Fs.writeFileSync(
           concat(toDirectory, "LonaCollectionView.swift"),
           annotate(
             SwiftCollectionView.generate(
               config,
               options,
               options.swift,
               successfulComponentNames,
             ),
           ),
           `utf8,
         );
       };

       Config.Find.files(config, "**/*.png")
       |> List.map(file => {
            let fromRelativePath =
              Path.relative(~from=fromDirectory, ~to_=file, ());
            let outputPath = Path.join([|toDirectory, fromRelativePath|]);
            Js.log(
              Path.join([|workspace, fromRelativePath|])
              ++ "=>"
              ++ Path.join([|output, fromRelativePath|]),
            );
            copySync(file, outputPath);
          })
       |> Js.Promise.resolve;
     });
};
switch (scanResult.command) {
| Workspace(target, workspacePath, outputPath) =>
  convertWorkspace(target, workspacePath, outputPath) |> ignore
| Component(target, input) =>
  Config.load(platformId(target), options, input, "")
  |> Js.Promise.then_(config => {
       convertComponent(config, target, input) |> Js.log;
       Js.Promise.resolve();
     })
  |> ignore
| Colors(target, input) =>
  let initialWorkspaceSearchPath =
    switch (input) {
    | File(path) => path
    | Stdin => Process.cwd()
    };

  Config.load(platformId(target), options, initialWorkspaceSearchPath, "")
  |> Js.Promise.then_((config: Config.t) =>
       switch (input) {
       | Stdin =>
         getStdin()
         |> Js.Promise.then_(contents => {
              let config = {
                ...config,
                colorsFile: {
                  path: "__stdin__",
                  contents: Color.parseFile(contents),
                },
              };

              convertColors(target, config) |> Js.log;

              Js.Promise.resolve();
            })
       | File(_) =>
         convertColors(target, config) |> Js.log;

         Js.Promise.resolve();
       }
     )
  |> ignore;
| Shadows(target, input) =>
  let initialWorkspaceSearchPath =
    switch (input) {
    | File(path) => path
    | Stdin => Process.cwd()
    };

  Config.load(platformId(target), options, initialWorkspaceSearchPath, "")
  |> Js.Promise.then_((config: Config.t) =>
       switch (input) {
       | File(_) =>
         convertShadows(target, config) |> Js.log;

         Js.Promise.resolve();
       | Stdin =>
         getStdin()
         |> Js.Promise.then_(contents => {
              let config = {
                ...config,
                shadowsFile: {
                  path: "__stdin__",
                  contents: Shadow.parseFile(contents),
                },
              };

              convertShadows(target, config) |> Js.log;

              Js.Promise.resolve();
            })
       }
     )
  |> ignore;
| Types(target, inputPath) =>
  let contents = Node.Fs.readFileSync(inputPath, `utf8);
  let jsonContents = Serialization.convert(contents, "types", "json");
  convertTypes(target, jsonContents) |> Js.log;
| Logic(target, input, initialWorkspaceSearchPath) =>
  let decode = contents: LogicAst.syntaxNode => {
    let jsonContents = Serialization.convert(contents, "logic", "json");
    let json = jsonContents |> Js.Json.parseExn;
    LogicAst.Decode.syntaxNode(json);
  };

  Config.load(platformId(target), options, initialWorkspaceSearchPath, "")
  |> Js.Promise.then_((config: Config.t) =>
       switch (input) {
       | File(inputPath) =>
         let contents = Node.Fs.readFileSync(inputPath, `utf8);
         decode(contents) |> convertLogic(target, config) |> Js.log;
         Js.Promise.resolve();
       | Stdin =>
         getStdin()
         |> Js.Promise.then_(contents => {
              let program = decode(contents);
              let config = {
                ...config,
                logicFiles: [
                  {path: "__stdin__", contents: program},
                  ...config.logicFiles,
                ],
              };
              program |> convertLogic(target, config) |> Js.log;

              Js.Promise.resolve();
            })
       }
     )
  |> ignore;
| TextStyles(target, input) =>
  let initialWorkspaceSearchPath =
    switch (input) {
    | File(path) => path
    | Stdin => Process.cwd()
    };

  Config.load(platformId(target), options, initialWorkspaceSearchPath, "")
  |> Js.Promise.then_((config: Config.t) =>
       switch (input) {
       | File(_) =>
         convertTextStyles(target, config) |> Js.log;

         Js.Promise.resolve();
       | Stdin =>
         getStdin()
         |> Js.Promise.then_(contents => {
              let config = {
                ...config,
                textStylesFile: {
                  path: "__stdin__",
                  contents: TextStyle.parseFile(contents),
                },
              };

              convertTextStyles(target, config) |> Js.log;

              Js.Promise.resolve();
            })
       }
     )
  |> ignore;
| Config(workspacePath) =>
  Config.load(Types.ReactDOM, options, workspacePath, "")
  |> Js.Promise.then_((config: Config.t) => {
       Js.log(config |> Config.toJson(version) |> Json.stringify);
       Js.Promise.resolve();
     })
  |> ignore
| Version => Js.log(version)
};