FROM gcc:13 AS build
WORKDIR /app
COPY server.c .
RUN gcc -O2 -o plms server.c

FROM debian:stable-slim
WORKDIR /app
COPY --from=build /app/plms .
COPY public ./public
CMD ["./plms"]
