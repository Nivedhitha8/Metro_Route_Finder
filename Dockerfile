FROM gcc:13 AS build
WORKDIR /src
COPY metro.c .
RUN gcc -O2 -o metro metro.c

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /src/metro .
COPY data data
COPY frontend frontend
ENV PORT=10000
EXPOSE 10000
CMD ["./metro"]
